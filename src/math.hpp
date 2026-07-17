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
b32 contains(Rect rect, V2 point) {
    return point.x >= rect.x && point.x < rect.x + rect.w && point.y >= rect.y && point.y < rect.y + rect.h;
}

} // namespace math
