#pragma once
#include <string.h>
#include <stdarg.h>
#include <stdio.h> // vsnprintf only — the backend of string::format

#include "core.hpp"
#include "arena.hpp"

// Helpers for the core String type (the type itself is global, in core.hpp).
// Strings are byte slices; everything here is byte-oriented and safe on the
// ZII empty string.

namespace string {

// Null-terminated copy for OS/library boundaries — C strings never travel
// further inward than the call site that needs them.
fn const char* to_cstr(arena::Arena* arena, String s) {
    char* cstr = (char*)arena::allocate_raw(arena, s.len + 1, 1); // zeroed: terminator included
    if (s.len) memcpy(cstr, s.data, s.len);
    return cstr;
}

namespace {

fn bool is_digit(char c) { return c >= '0' && c <= '9'; }

} // namespace

// printf-style formatting into an arena string. String arguments go through
// the length-prefix idiom: format(a, "%.*s!", (int)s.len, s.data). The
// two-pass vsnprintf measures first, so the result is never truncated.
__attribute__((format(printf, 2, 3)))
fn String format(arena::Arena* arena, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    va_list measure_args;
    va_copy(measure_args, args);
    int len = vsnprintf(0, 0, fmt, measure_args);
    va_end(measure_args);
    if (len <= 0) {
        va_end(args);
        return {}; // ZII: encoding error or empty result -> empty string
    }
    char* data = (char*)arena::allocate_raw(arena, (usize)len + 1, 1); // +1: vsnprintf's terminator
    if (data) vsnprintf(data, (usize)len + 1, fmt, args);
    va_end(args);
    if (!data) return {}; // ZII: arena full
    return {(usize)len, data};
}

// Decimal integer as an arena string — format's most common case, named.
// Arena-backed on purpose: callers routinely hand the result to code that
// stores String views (ui::data), so a stack buffer here would dangle.
fn String from_int(arena::Arena* arena, i64 value) { return format(arena, "%lld", (long long)value); }

fn b32 starts_with(String s, String prefix) {
    return s.len >= prefix.len && (prefix.len == 0 || memcmp(s.data, prefix.data, prefix.len) == 0);
}

struct ParseF32Result {
    f32 value;
    b32 ok;
};

// Hand-rolled float parse: [+-] digits [ '.' digits ] [ (e|E) [+-] digits ],
// at least one mantissa digit, and the whole input must be consumed — so
// "1444.11.11" is not a number. Accumulates in f64, which can differ from a
// correctly-rounded strtof in the last ULP; game data does not care.
fn ParseF32Result parse_f32(String text) {
    usize i    = 0;
    f64   sign = 1.0;
    if (i < text.len && (text.data[i] == '+' || text.data[i] == '-')) {
        if (text.data[i] == '-') sign = -1.0;
        i += 1;
    }

    f64   value  = 0.0;
    usize digits = 0;
    while (i < text.len && is_digit(text.data[i])) {
        value = value * 10.0 + (f64)(text.data[i] - '0');
        i += 1;
        digits += 1;
    }
    if (i < text.len && text.data[i] == '.') {
        i += 1;
        f64 scale = 0.1;
        while (i < text.len && is_digit(text.data[i])) {
            value += (f64)(text.data[i] - '0') * scale;
            scale *= 0.1;
            i += 1;
            digits += 1;
        }
    }
    if (digits == 0) return {}; // ZII: not a number

    if (i < text.len && (text.data[i] == 'e' || text.data[i] == 'E')) {
        i += 1;
        bool exp_negative = false;
        if (i < text.len && (text.data[i] == '+' || text.data[i] == '-')) {
            exp_negative = text.data[i] == '-';
            i += 1;
        }
        i32   exp        = 0;
        usize exp_digits = 0;
        while (i < text.len && is_digit(text.data[i])) {
            // Clamp: f32 saturates to inf/0 far before 1000 anyway.
            if (exp < 1000) exp = exp * 10 + (text.data[i] - '0');
            i += 1;
            exp_digits += 1;
        }
        if (exp_digits == 0) return {}; // "1e" is not a number
        for (i32 e = 0; e < exp; ++e) {
            value = exp_negative ? value / 10.0 : value * 10.0;
        }
    }

    if (i != text.len) return {}; // trailing junk — not a number
    return {.value = (f32)(sign * value), .ok = true};
}

fn String substring(String base, usize start, usize length) {
    start  = min(start, base.len);
    length = min(length, base.len - start);
    return {
        length,
        base.data + start,
    };
}

} // namespace string
