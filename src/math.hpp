#pragma once
#include "core.hpp"
#include <compare>

namespace math {
// Two dimensional vector
struct V2 {
    f32 x;
    f32 y;

    auto operator<=>(const V2&) const = default;

    // TODO: Numerical operations with other vectors and scalars
    V2 operator-(V2 other) { return {this->x - other.x, this->y - other.y}; }
    V2 operator/(f32 v) { return {this->x / v, this->y / v}; }
};

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

fn V2 splat(f32 v) { return {v, v}; }

fn Rect rect_with_center_and_size(V2 pos, V2 size) {
    auto corner = pos - size / 2;
    return {corner.x, corner.y, size.x, size.y};
}

} // namespace math
