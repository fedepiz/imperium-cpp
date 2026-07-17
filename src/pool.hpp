#pragma once

#include "core.hpp"

// Fixed-size pool with generational keys. It owns its own memory via a
// compile-time capacity, so ZII holds fully: the all-zero pool is valid and
// empty — no arena, no make_ call; it can live as a global or embedded in a
// system struct.
//
// K is anything key-like: a POD with two u32 fields, slot and generation,
// whose all-zero value means null. The pool defines no key type — each
// consumer declares its own (FontKey, TextureKey, ...) and binds it into the
// pool's type, so keys for different stores can't mix.
//
// Slot 0 is the permanently-zeroed dummy: pool[key] on a null, stale, or
// garbage key returns its value, so lookups never yield null and misses are
// safe to read (don't write through a miss — it dirties the dummy).
// Liveness is generation parity — odd = live, even = free — which is what
// makes stale keys detectable: freeing a slot moves its generation past
// every key still in the wild.
//
// alloc() on a full pool returns the zero key, not a trap: fullness is a
// legitimate, testable state (caches). Slots come back zeroed, arena-style.

namespace pool {

template <typename K> b32 is_nil_key(K key) {
    return key.slot == 0 && key.generation == 0;
}
    
template <typename K, typename V, const usize N> struct Pool;

// Sanctioned non-arithmetic operator overloads: skipping dead slots is what
// the iterator exists to hide. Visits live entries only, dummy excluded.
template <typename K, typename V, const usize N> struct Iter {
    Pool<K, V, N>* pool;
    u32            slot;

    typename Pool<K, V, N>::Entry& operator*() const { return pool->entries[slot]; }
    bool                           operator!=(Iter other) const { return pool != other.pool || slot != other.slot; }
    void                           operator++() {
        slot += 1;
        while (slot < pool->used && (pool->entries[slot].key.generation & 1) == 0)
            slot += 1;
    }
};

template <typename K, typename V, const usize N> struct Pool {
    struct Entry {
        K key; // key.slot == this entry's index while live; generation parity tracks liveness
        V value;
    };

    Entry entries[N];   // entries[0] is the dummy — permanently zero, never handed out
    u32   free_list[N]; // freed slots awaiting reuse
    u32   free_count;
    u32   used; // high-water mark, dummy included: slots ever touched
    u32   live_count;

    // The live value, or the dummy on any miss — never invalid.
    V& operator[](K key) {
        u32  slot = key.slot;
        bool live = slot < N && (key.generation & 1) && this->entries[slot].key.generation == key.generation;
        return this->entries[live ? slot : 0].value;
    }

    const V& operator[](K key) const {
        u32  slot = key.slot;
        bool live = slot < N && (key.generation & 1) && this->entries[slot].key.generation == key.generation;
        return this->entries[live ? slot : 0].value;
    }

    Iter<K, V, N> begin() {
        u32 slot = this->used ? 1 : 0;
        while (slot < this->used && (this->entries[slot].key.generation & 1) == 0)
            slot += 1;
        return {this, slot};
    }
    Iter<K, V, N> end() { return {this, this->used}; }
};

// Zero key when full. The claimed slot comes back zeroed, arena-style.
template <typename K, typename V, const usize N> K alloc(Pool<K, V, N>* pool) {
    u32 slot = 0;
    if (pool->free_count) {
        pool->free_count -= 1;
        slot = pool->free_list[pool->free_count];
    } else {
        if (pool->used == 0) pool->used = 1; // slot 0 is never handed out
        if (pool->used < N) {
            slot = pool->used;
            pool->used += 1;
        }
    }
    if (slot == 0) return {}; // full

    auto* entry           = &pool->entries[slot];
    u32   generation      = entry->key.generation + 1; // even -> odd: live
    *entry                = {};
    entry->key.slot       = slot;
    entry->key.generation = generation;
    pool->live_count += 1;
    return entry->key;
}

// alloc + assignment in one step; nil key (and nothing stored) when full.
template <typename K, typename V, const usize N> K insert(Pool<K, V, N>* pool, V value) {
    auto key = alloc(pool);
    if (!is_nil_key(key)) {
        pool->entries[key.slot].value = value;
    }
    return key;
}

// Frees only an exact live match; anything else — null, stale, out of range —
// is a no-op. Returns whether a slot was actually freed, for callers with
// external cleanup tied to the slot (GPU resources).
template <typename K, typename V, const usize N> b32 free(Pool<K, V, N>* pool, K key) {
    u32 slot = key.slot;
    if (slot >= N || (key.generation & 1) == 0 || pool->entries[slot].key.generation != key.generation) return false;
    pool->entries[slot].key.generation += 1; // odd -> even: dead; every key in the wild goes stale
    pool->free_list[pool->free_count] = slot;
    pool->free_count += 1;
    pool->live_count -= 1;
    return true;
}

} // namespace pool
