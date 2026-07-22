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

constexpr usize KB = 1024;
constexpr usize MB = 1024 * KB;
constexpr usize GB = 1024 * MB;

// fn — the project function keyword: every free-function definition is
// written `fn <return> name(...)`. Expands to inline, so a module can land in
// more than one TU of a binary (the boundary TU) without duplicate-symbol
// errors. Not used on: main (may not be inline), declarations (module
// contracts stay plain), and boundary-TU implementations of a module's API —
// those must keep a single strong definition other TUs can link against.
#define fn inline

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

// Small generic utilities.

template <typename T> fn T min(T a, T b) { return a < b ? a : b; }
template <typename T> fn T max(T a, T b) { return a > b ? a : b; }
template <typename T> fn T clamp(T value, T low, T high) { return value < low ? low : (value > high ? high : value); }

// Basic aggregate types: arrays and slices

template <typename T, const usize N> struct Array {
    T data[N];

    T& operator[](usize idx) {
        ASSERT(idx < N);
        return this->data[idx];
    }

    const T& operator[](usize idx) const {
        ASSERT(idx < N);
        return this->data[idx];
    }

    usize len() const { return N; }

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

template <const usize N> struct DynString;

// String — an immutable view of bytes: pointer + length, not null-terminated.
// Distinct from Slice<char>: its bytes can never be written through it (const
// data), and it converts implicitly from a null-terminated literal
// (String name = "Roma";), from Slice<char>, and from DynString<N>, so
// builders assemble into a mutable slice or inline buffer and the result
// travels as String.
struct String {
    usize       len;
    const char* data;

    String() = default;
    String(usize len, const char* data) : len{len}, data{data} {}
    String(const char* cstr) : len{cstr ? strlen(cstr) : 0}, data{cstr} {}
    String(Slice<char> s) : len{s.len}, data{s.data} {}
    template <const usize N> String(const DynString<N>& s);

    const char& operator[](usize idx) const {
        ASSERT(idx < this->len);
        return this->data[idx];
    }

    const char* begin() const { return this->data; }
    const char* end() const { return this->data + this->len; }
};

// Value comparison: same bytes, wherever they live (!= comes from the C++20
// rewrite). Two bare literals compare as pointers — wrap one side in String.
fn bool operator==(String a, String b) {
    if (a.len != b.len) return false;
    if (a.len == 0 || a.data == b.data) return true;
    return memcmp(a.data, b.data, a.len) == 0;
}

// Fixed-capacity dynamic array — Vec's shape on embedded storage, no arena.
// ZII: the all-zero DynArray is a valid empty array. It cannot grow: push
// reports success with a b32 and leaves the array untouched when full.
template <typename T, const usize N> struct DynArray {
    usize len;
    T     data[N];

    T& operator[](usize idx) {
        ASSERT(idx < this->len);
        return this->data[idx];
    }

    const T& operator[](usize idx) const {
        ASSERT(idx < this->len);
        return this->data[idx];
    }

    usize capacity() const { return N; }

    T* begin() { return this->data; }
    T* end() { return this->data + this->len; }

    const T* begin() const { return this->data; }
    const T* end() const { return this->data + this->len; }
};

template <typename T, const usize N> fn b32 push(DynArray<T, N>* array, T value) {
    if (array->len == N) return false;
    array->data[array->len] = value;
    array->len += 1;
    return true;
}

template <typename T, const usize N> fn void pop(DynArray<T, N>* array) {
    ASSERT(array->len > 0);
    array->len -= 1;
}

template <typename T, const usize N> fn void clear(DynArray<T, N>* array) {
    array->len = 0;
}

// View of the current contents; valid until the array is popped or cleared.
template <typename T, const usize N> fn Slice<T> slice(DynArray<T, N>* array) {
    return {array->len, array->data};
}

// All-or-nothing like push: when the items don't all fit, nothing is appended.
template <typename T, const usize N> fn b32 append(DynArray<T, N>* array, Slice<T> items) {
    if (items.len > N - array->len) return false;
    if (items.len) memcpy(array->data + array->len, items.data, items.len * sizeof(T));
    array->len += items.len;
    return true;
}

// String is not Slice<char> (its bytes are const); char buffers still append
// it — the source is only read.
template <const usize N> fn b32 append(DynArray<char, N>* array, String items) {
    return append(array, Slice<char>{items.len, (char*)items.data});
}

// Fixed-capacity inline string — DynArray's shape on bytes, String's read
// surface: bytes enter through push/append and leave through the implicit
// String conversion. ZII: the all-zero DynString is the empty string. It
// cannot grow: push and append report success with a b32 and leave the
// contents untouched when the bytes don't fit.
template <const usize N> struct DynString {
    usize len;
    char  data[N];

    // Bytes copy IN — the mirror of the String conversion out. Truncates to
    // capacity silently; append is the variant that refuses instead.
    // memmove, so assigning a view of this same buffer is safe. Takes
    // String, not DynString, so it is no copy special member and the type
    // stays trivially copyable.
    DynString& operator=(String s) {
        this->len = min(s.len, N);
        if (this->len) memmove(this->data, s.data, this->len);
        return *this;
    }

    const char& operator[](usize idx) const {
        ASSERT(idx < this->len);
        return this->data[idx];
    }

    usize capacity() const { return N; }

    const char* begin() const { return this->data; }
    const char* end() const { return this->data + this->len; }
};

// The DynString -> String conversion: a view of THIS buffer, valid until
// the next push/append/clear. A copied DynString carries its own bytes —
// convert from the copy you keep.
template <const usize N> String::String(const DynString<N>& s) : len{s.len}, data{s.data} {}

template <const usize N> fn b32 push(DynString<N>* s, char c) {
    if (s->len == N) return false;
    s->data[s->len] = c;
    s->len += 1;
    return true;
}

// All-or-nothing like push: when the bytes don't all fit, nothing is appended.
template <const usize N> fn b32 append(DynString<N>* s, String text) {
    if (text.len > N - s->len) return false;
    if (text.len) memcpy(s->data + s->len, text.data, text.len);
    s->len += text.len;
    return true;
}

template <const usize N> fn void clear(DynString<N>* s) { s->len = 0; }

#endif
