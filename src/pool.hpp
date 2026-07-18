#pragma once

#include "core.hpp"

// Fixed-size pool with generational keys. It owns its own memory via a
// compile-time capacity, so ZII holds fully: the all-zero pool is valid and
// empty — no arena, no make_ call; it can live as a global or embedded in a
// system struct.
//
// A key is an opaque u64: a POD wrapping a single u64 `value` whose all-zero
// state means null (the Key concept below). The pool defines no key type —
// each consumer declares its own (FontId, TextureId, ...) and binds it into
// the pool's type, so keys for different stores can't mix. How slot and
// generation pack into the value is private to this file; a key also drops
// straight into hashtable::Table (u64 keys, key 0 reserved — the nil keys
// coincide).
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

// The key contract: a POD wrapping one u64 `value`, zero = null.
template <typename K>
concept Key = sizeof(K) == sizeof(u64) && requires(K key) {
    key.value;
    K{u64{}};
};

template <Key K> fn b32 is_nil_key(K key) {
    return key.value == 0;
}

namespace {

// The packed layout — private to the pool: slot in the low 32 bits,
// generation in the high 32. The nil key falls out: zero value = slot 0,
// generation 0. maybe_unused: only the templates below call these, so a
// standalone parse of this module sees them unused.
[[maybe_unused]] fn u32 key_slot(u64 value) { return (u32)value; }
[[maybe_unused]] fn u32 key_generation(u64 value) { return (u32)(value >> 32); }
[[maybe_unused]] fn u64 key_pack(u32 slot, u32 generation) { return (u64)slot | ((u64)generation << 32); }

} // namespace

template <Key K, typename V, const usize N> struct Pool;

// Sanctioned non-arithmetic operator overloads: skipping dead slots is what
// the iterator exists to hide. Visits live entries only, dummy excluded.
template <Key K, typename V, const usize N> struct Iter {
    Pool<K, V, N>* pool;
    u32            slot;

    typename Pool<K, V, N>::Entry& operator*() const { return pool->entries[slot]; }
    bool                           operator!=(Iter other) const { return pool != other.pool || slot != other.slot; }
    void                           operator++() {
        slot += 1;
        while (slot < pool->used && (key_generation(pool->entries[slot].key.value) & 1) == 0)
            slot += 1;
    }
};

template <Key K, typename V, const usize N> struct Pool {
    struct Entry {
        K key; // packs this entry's index while live; generation parity tracks liveness
        V value;
    };

    Entry entries[N];   // entries[0] is the dummy — permanently zero, never handed out
    u32   free_list[N]; // freed slots awaiting reuse
    u32   free_count;
    u32   used; // high-water mark, dummy included: slots ever touched
    u32   live_count;

    // The live value, or the dummy on any miss — never invalid. A live key
    // matches its entry's key bit-for-bit, so one compare covers slot
    // identity and generation.
    V& operator[](K key) {
        u32  slot = key_slot(key.value);
        bool live = slot < N && (key_generation(key.value) & 1) && this->entries[slot].key.value == key.value;
        return this->entries[live ? slot : 0].value;
    }

    const V& operator[](K key) const {
        u32  slot = key_slot(key.value);
        bool live = slot < N && (key_generation(key.value) & 1) && this->entries[slot].key.value == key.value;
        return this->entries[live ? slot : 0].value;
    }

    Iter<K, V, N> begin() {
        u32 slot = this->used ? 1 : 0;
        while (slot < this->used && (key_generation(this->entries[slot].key.value) & 1) == 0)
            slot += 1;
        return {this, slot};
    }
    Iter<K, V, N> end() { return {this, this->used}; }
};

// Zero key when full. The claimed slot comes back zeroed, arena-style.
template <Key K, typename V, const usize N> fn K alloc(Pool<K, V, N>* pool) {
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

    auto* entry      = &pool->entries[slot];
    u32   generation = key_generation(entry->key.value) + 1; // even -> odd: live
    *entry           = {};
    entry->key       = K{key_pack(slot, generation)};
    pool->live_count += 1;
    return entry->key;
}

// alloc + assignment in one step; nil key (and nothing stored) when full.
template <Key K, typename V, const usize N> fn K insert(Pool<K, V, N>* pool, V value) {
    auto key = alloc(pool);
    if (!is_nil_key(key)) {
        pool->entries[key_slot(key.value)].value = value;
    }
    return key;
}

// Frees only an exact live match; anything else — null, stale, out of range —
// is a no-op. Returns whether a slot was actually freed, for callers with
// external cleanup tied to the slot (GPU resources).
template <Key K, typename V, const usize N> fn b32 free(Pool<K, V, N>* pool, K key) {
    u32 slot = key_slot(key.value);
    if (slot >= N || (key_generation(key.value) & 1) == 0 || pool->entries[slot].key.value != key.value) return false;
    pool->entries[slot].key = K{key_pack(slot, key_generation(key.value) + 1)}; // odd -> even: dead; every key in the wild goes stale
    pool->free_list[pool->free_count] = slot;
    pool->free_count += 1;
    pool->live_count -= 1;
    return true;
}

} // namespace pool
