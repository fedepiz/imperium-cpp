#pragma once
#include <string.h>

#include "core.hpp"
#include "arena.hpp"

// Arena-backed open-addressing hash table with u64 keys — the codebase's
// hash map. Open addressing means entries relocate on growth: value pointers
// from get/put are invalidated by the next put. (A future address-stable
// binned variant would live beside this one, not replace it.)
//
// Keys are expected to already be hashes (FNV ids and the like); a mixer is
// still applied so clustered keys probe well. Key 0 is reserved as the empty
// slot marker — ZII: the all-zero table is a valid empty table, and reading
// from it is safe. Writing needs a wired arena, like vec.

namespace hashtable {

template <typename V> struct Entry {
    u64 key;
    V*  value;
};

template <typename V> struct Hashtable {
    arena::Arena* arena;
    usize         count;
    usize         capacity; // power of two; 0 = unallocated
    u64*          keys;     // 0 = empty slot
    V*            values;

    struct Iter {
        Hashtable* table;
        usize  index;

        b32 operator!=(const Iter& other) const { return this->index != other.index; }
        Entry<V> operator*() const { return {this->table->keys[this->index], &this->table->values[this->index]}; }
        void     operator++() {
            do {
                this->index += 1;
            } while (this->index < this->table->capacity && this->table->keys[this->index] == 0);
        }
    };

    Iter begin() {
        usize index = 0;
        while (index < this->capacity && this->keys[index] == 0) index += 1;
        return {this, index};
    }
    Iter end() { return {this, this->capacity}; }
};

// Final mixer (murmur3 finalizer): spreads whatever structure the caller's
// keys have across the whole word so masked slots don't cluster. Public so
// non-template code can pre-mix; only the templates below need it here.
fn u64 mix(u64 key) {
    key ^= key >> 33;
    key *= 0xff51afd7ed558ccdull;
    key ^= key >> 33;
    return key;
}

template <typename V> fn void grow(Hashtable<V>* table, usize new_capacity) {
    u64* old_keys     = table->keys;
    V*   old_values   = table->values;
    usize old_capacity = table->capacity;

    table->keys     = (u64*)arena::allocate_raw(table->arena, new_capacity * sizeof(u64), alignof(u64));
    table->values   = arena::allocate<V>(table->arena, new_capacity);
    table->capacity = new_capacity;

    usize mask = new_capacity - 1;
    for (usize i = 0; i < old_capacity; ++i) {
        if (old_keys[i] == 0) continue;
        usize slot = mix(old_keys[i]) & mask;
        while (table->keys[slot] != 0) slot = (slot + 1) & mask;
        table->keys[slot]   = old_keys[i];
        table->values[slot] = old_values[i];
    }
    // The old block stays behind in the arena until it is reset — bulk lifetime.
}

template <typename V> fn Hashtable<V> make_table(arena::Arena* arena, usize capacity) {
    Hashtable<V> result = {};
    result.arena    = arena;
    usize rounded = 8;
    while (rounded < capacity) rounded *= 2;
    if (capacity) grow(&result, rounded);
    return result;
}

// True when inserting `additional` NEW keys from the current state would
// trigger at least one reallocation. The one home of the load-factor rule:
// put grows through here, and callers that must never grow (fixed-budget
// caches that purge instead) ask here before a batch.
template <typename V> fn b32 would_grow(const Hashtable<V>* table, usize additional) {
    return (table->count + additional) * 4 > table->capacity * 3;
}

// Pointer to the value for key, or null when absent. Safe on the ZII table.
template <typename V> fn V* get(Hashtable<V>* table, u64 key) {
    ASSERT(key != 0);
    if (table->count == 0) return 0;
    usize mask = table->capacity - 1;
    usize slot = mix(key) & mask;
    while (table->keys[slot] != 0) {
        if (table->keys[slot] == key) return &table->values[slot];
        slot = (slot + 1) & mask;
    }
    return 0;
}

// Insert-or-get: the value slot for key, zeroed when newly inserted. The
// pointer is valid until the next put (growth relocates).
template <typename V> fn V* put(Hashtable<V>* table, u64 key) {
    ASSERT(key != 0);
    // Grow at 3/4 load — before probing, so the probe below always terminates.
    if (would_grow(table, 1)) {
        grow(table, table->capacity ? table->capacity * 2 : 8);
    }
    usize mask = table->capacity - 1;
    usize slot = mix(key) & mask;
    while (table->keys[slot] != 0) {
        if (table->keys[slot] == key) return &table->values[slot];
        slot = (slot + 1) & mask;
    }
    table->keys[slot]   = key;
    table->values[slot] = {};
    table->count += 1;
    return &table->values[slot];
}

template <typename V> fn b32 remove(Hashtable<V>* table, u64 key) {
    ASSERT(key != 0);
    if (table->count == 0) return false;
    usize mask = table->capacity - 1;
    usize hole = mix(key) & mask;
    while (table->keys[hole] != key) {
        if (table->keys[hole] == 0) return false;
        hole = (hole + 1) & mask;
    }
    // Backward-shift deletion: pull each displaced follower of the probe
    // chain into the hole instead of leaving a tombstone.
    usize probe = hole;
    while (true) {
        table->keys[hole] = 0;
        while (true) {
            probe = (probe + 1) & mask;
            if (table->keys[probe] == 0) {
                table->count -= 1;
                return true;
            }
            usize ideal = mix(table->keys[probe]) & mask;
            // Movable into the hole only if its ideal slot is at or before
            // the hole (cyclically) — otherwise it belongs after it.
            if (((probe - ideal) & mask) >= ((probe - hole) & mask)) break;
        }
        table->keys[hole]   = table->keys[probe];
        table->values[hole] = table->values[probe];
        hole = probe;
    }
}

template <typename V> fn void clear(Hashtable<V>* table) {
    if (table->capacity) memset(table->keys, 0, table->capacity * sizeof(u64));
    table->count = 0;
}

} // namespace hashtable
