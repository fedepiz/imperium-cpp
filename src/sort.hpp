#pragma once
#include <string.h>

#include "core.hpp"
#include "arena.hpp"

// Sorting over slices, in place: the sorted data lands back in the slice.
// `less(a, b)` answers "does a sort strictly before b". `stable` keeps equal
// elements in input order at the cost of O(n) scratch borrowed from the
// caller's arena; `unstable` needs no scratch but scrambles ties.

namespace sort {

// Scratch sits behind a watermark, so nothing allocated here outlives the
// call.
template <typename T, typename Cmp> fn void stable(Slice<T> s, arena::Arena* scratch, Cmp less) {
    if (s.len < 2) return;

    arena::ScratchArena watermark(scratch);
    T* buf = arena::allocate<T>(scratch, s.len);
    ASSERT(buf); // an exhausted scratch arena is a budgeting bug

    // Bottom-up merge: each pass doubles the sorted-run width, ping-ponging
    // between the slice and the buffer. Ties take the left run — that is the
    // stability. After the final pass `src` holds the sorted data; copy back
    // when an odd number of passes left it in the buffer.
    T* src = s.data;
    T* dst = buf;
    for (usize width = 1; width < s.len; width *= 2) {
        for (usize lo = 0; lo < s.len; lo += width * 2) {
            usize mid = min(lo + width, s.len);
            usize hi  = min(lo + width * 2, s.len);
            usize a   = lo;
            usize b   = mid;
            usize out = lo;
            while (a < mid && b < hi) dst[out++] = less(src[b], src[a]) ? src[b++] : src[a++];
            while (a < mid) dst[out++] = src[a++];
            while (b < hi) dst[out++] = src[b++];
        }
        T* tmp = src;
        src    = dst;
        dst    = tmp;
    }
    if (src != s.data) memcpy(s.data, src, s.len * sizeof(T));
}

// Internal — callers go through unstable. Restores the max-heap property for
// the subtree rooted at `root` within data[0, len).
template <typename T, typename Cmp> fn void sift_down(T* data, usize root, usize len, Cmp less) {
    for (;;) {
        usize child = root * 2 + 1;
        if (child >= len) return;
        if (child + 1 < len && less(data[child], data[child + 1])) child++;
        if (!less(data[root], data[child])) return;
        T tmp       = data[root];
        data[root]  = data[child];
        data[child] = tmp;
        root = child;
    }
}

// Heapsort: build a max-heap, then repeatedly move the root behind the
// shrinking heap. The heap reorders equal elements — that is the
// "unstable".
template <typename T, typename Cmp> fn void unstable(Slice<T> s, Cmp less) {
    if (s.len < 2) return;
    for (usize i = s.len / 2; i-- > 0;) sift_down(s.data, i, s.len, less);
    for (usize end = s.len - 1; end > 0; end--) {
        T tmp       = s.data[0];
        s.data[0]   = s.data[end];
        s.data[end] = tmp;
        sift_down(s.data, 0, end, less);
    }
}

} // namespace sort
