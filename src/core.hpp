#ifndef CORE_CPP
#define CORE_CPP

// Basic type aliases
#include <cstddef>
#include <cstdint>
#include <string.h>
using b32 = bool;

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

using i8  = std::int8_t;
using i16 = std::int16_t;
using i32 = std::int32_t;
using i64 = std::int64_t;

using f32 = float;
using f64 = double;

using usize = std::size_t;

// ASSERT — programmer errors and invariants: traps into the debugger.
// LOG(fmt, ...) — printf-style logging to stderr; fmt must be a string literal.
// Both compile to nothing unless ASSERT_ENABLE / LOG_ENABLE is defined (by the
// root file or the build script), so neither may carry side effects the
// program depends on.

#ifdef ASSERT_ENABLE
#if defined(_MSC_VER)
#define ASSERT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) { __debugbreak(); }                                                                               \
    } while (0)
#else
#define ASSERT(cond)                                                                                                   \
    do {                                                                                                               \
        if (!(cond)) { __builtin_trap(); }                                                                             \
    } while (0)
#endif
#else
#define ASSERT(cond) ((void)0)
#endif

#ifdef LOG_ENABLE
#include <stdio.h>
#define LOG(fmt, ...) fprintf(stderr, fmt "\n" __VA_OPT__(, ) __VA_ARGS__)
#else
#define LOG(fmt, ...) ((void)0)
#endif

// Basic aggregate types: arrays and slices

template <typename T, const usize N> struct Array {
    T data[N];

    T& operator[](usize idx) const {
        ASSERT(idx < N);
        return this->data[idx];
    }

    T* begin() { return this->data; }
    T* end() { return this->data + N; }

    const T* begin() const { return this->data; }
    const T* end() const { return this->data + N; }
};

template <typename T> struct Slice {
    usize len;
    T*    data;

    T& operator[](usize idx) const {
        ASSERT(idx < this->len);
        return this->data[idx];
    }

    T* begin() { return this->data; }
    T* end() { return this->data + this->len; }

    const T* begin() const { return this->data; }
    const T* end() const { return this->data + this->len; }
};

// Slice<char> is String. The specialization exists to add the one thing the
// generic slice must not have: implicit construction from a null-terminated
// string (String name = "Roma";) — a sanctioned one-off implicitness. The
// constructors cost the aggregate, so char slices brace-init positionally:
// {len, data}. Keep the shared body in sync with the primary template.
template <> struct Slice<char> {
    usize len;
    char* data;

    Slice() = default;
    Slice(usize len, char* data) : len{len}, data{data} {}
    Slice(const char* cstr) : len{cstr ? strlen(cstr) : 0}, data{(char*)cstr} {}

    char& operator[](usize idx) const {
        ASSERT(idx < this->len);
        return this->data[idx];
    }

    char* begin() { return this->data; }
    char* end() { return this->data + this->len; }

    const char* begin() const { return this->data; }
    const char* end() const { return this->data + this->len; }
};

using String = Slice<char>;

#endif
