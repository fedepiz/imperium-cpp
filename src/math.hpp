#pragma once
#include "core.hpp"
#include <compare>

namespace math {
// Two dimensional vector
struct V2 {
    f32 x;
    f32 y;

    auto operator<=>(const V2&) const = default;

    V2 operator+(V2 other) { return {this->x + other.x, this->y + other.y}; }
    V2 operator-(V2 other) { return {this->x - other.x, this->y - other.y}; }
    V2 operator*(f32 v) { return {this->x * v, this->y * v}; }
    V2 operator/(f32 v) { return {this->x / v, this->y / v}; }
};

// ZII: zero is transparent black.
struct Color {
    u8 r, g, b, a;

    auto operator<=>(const Color&) const = default;
};

inline constexpr Color BLACK     = {0, 0, 0, 255};
inline constexpr Color WHITE     = {255, 255, 255, 255};
inline constexpr Color RED       = {230, 41, 55, 255};
inline constexpr Color GREEN     = {0, 228, 48, 255};
inline constexpr Color BLUE      = {0, 121, 241, 255};
inline constexpr Color DARK_GRAY = {80, 80, 80, 255};

struct Rect {
    f32 x;
    f32 y;
    f32 w;
    f32 h;

    auto operator<=>(const Rect&) const = default;
};

// Half-open on both axes — [x, x+w) — so adjacent rects never double-claim a
// point, and the ZII empty rect contains nothing.
fn b32 contains(Rect rect, V2 point) {
    return point.x >= rect.x && point.x < rect.x + rect.w && point.y >= rect.y && point.y < rect.y + rect.h;
}

fn Rect intersect(Rect a, Rect b) {
    f32 x = max(a.x, b.x);
    f32 y = max(a.y, b.y);
    return {x, y, max(min(a.x + a.w, b.x + b.w) - x, 0.0f), max(min(a.y + a.h, b.y + b.h) - y, 0.0f)};
}

constexpr fn V2 splat(f32 v) { return {v, v}; }

fn Rect rect_with_center_and_size(V2 pos, V2 size) {
    auto corner = pos - size / 2;
    return {corner.x, corner.y, size.x, size.y};
}

fn Rect resize_by(Rect rect, V2 size) {
    rect.x -= size.x / 2.0;
    rect.y -= size.y / 2.0;
    rect.w += size.x;
    rect.h += size.y;
    return rect;
}

} // namespace math
