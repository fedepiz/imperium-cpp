#pragma once
#include <math.h>
#include <compare>

#include "../core.hpp"
#include "../arena.hpp"
#include "../vec.hpp"
#include "../hashtable.hpp"
#include "../math.hpp"

// Small, renderer-independent, Clay-inspired immediate-mode UI layout.
// Ported from the Rust ui crate's layout.rs; the algorithm and constants
// match it so behavior (and element ids) stay identical.
//
// Usage mirrors the Rust closure API — lambdas are passed as template
// callable parameters and invoked during the call, never stored:
//
//     ui::layout::Output* output = ui::layout::layout(&engine, input,
//         [&](String text, u16 size) { return measure(text, size); },
//         [&](ui::layout::Ui* u) {
//             ui::layout::add_with(u, {.width = ui::layout::grow()}, [&] {
//                 ui::layout::Sense s = ui::layout::add(u, {.id = "btn"});
//             });
//         });
//
// Deferred features: richer typography, custom render commands, per-edge
// borders, visibility culling, aspect-ratio-preserving image sizing,
// floating attachment to arbitrary elements by id, transitions, drag and
// touch input with pointer capture, and debug inspection.

namespace ui {
namespace layout {
using namespace math; // math value types are pervasive here

// --------------------------------------------------------------- identifiers

fn u64 fnv1a(String text, u64 hash = 0xcbf29ce484222325ull) {
    for (usize i = 0; i < text.len; ++i) {
        hash ^= (u8)text.data[i];
        hash *= 0x100000001b3ull;
    }
    return hash;
}

fn u64 id_named(String name) { return max(fnv1a(name), (u64)1); }

// A name plus a counter, for elements declared in loops. Distinct from the
// plain name and from every other index. Also derives auto-ids for anonymous
// children (parent id + ordinal).
fn u64 id_child(u64 id, u32 index) { return max(id * 0x9e3779b185ebca87ull + (u64)index + 1, (u64)1); }

// Hash of a name; id 0 means "unset" (an anonymous element gets an auto-id
// derived from its parent). Conf sites spell it as a literal: .id = "btn",
// or .id = {"row", index} for loop elements.
struct ElementId {
    u64 value;

    ElementId() = default;
    constexpr ElementId(u64 raw) : value{raw} {}
    ElementId(String name) : value{id_named(name)} {}
    ElementId(const char* name) : value{id_named(String(name))} {}
    ElementId(String name, u32 index) : value{id_child(id_named(name), index)} {}
    ElementId(const char* name, u32 index) : value{id_child(id_named(String(name)), index)} {}

    auto operator<=>(const ElementId&) const = default;
};

inline constexpr ElementId ROOT = ElementId{(u64)-1};

/// Opaque handle to a renderer-owned texture; zero means no image.
struct ImageId {
    u64 value;

    auto operator<=>(const ImageId&) const = default;
};

// -------------------------------------------------------------- value types

struct TextMetrics {
    V2 size;
    // Distance from the top of the measured box to the drawing baseline.
    f32 baseline;
};

struct Padding {
    f32 left;
    f32 right;
    f32 top;
    f32 bottom;
};

fn Padding pad_all(f32 value) { return {value, value, value, value}; }
fn Padding pad_symmetric(f32 horizontal, f32 vertical) { return {horizontal, horizontal, vertical, vertical}; }

struct Border {
    f32   width;
    Color color;
    f32   radius;
};

enum class Direction : u32 {
    LeftToRight, // zero — ZII default, matching the Rust engine
    TopToBottom,
};

enum class Align : u32 {
    Start, // zero — ZII default
    Center,
    End,
};

// Per-axis sizing. ZII: the zero Size is Fit. The value field is the mode's
// payload: grow weight, pixels, or parent fraction.
enum class SizeMode : u32 {
    Fit, // zero — shrink to content
    Grow,
    Pixels,
    Parent,
};

struct Size {
    SizeMode mode;
    f32      value;
};

fn Size px(f32 value) { return {SizeMode::Pixels, value}; }
fn Size grow(f32 weight = 1.0f) { return {SizeMode::Grow, weight}; }
fn Size frac(f32 fraction) { return {SizeMode::Parent, fraction}; }

fn b32 is_grow(Size size) { return size.mode == SizeMode::Grow; }
fn f32 grow_weight(Size size) { return max(size.value, 0.0f); }

struct TextConf {
    String text;
    u16    size;
    Color  color;
    b32    wrap;
};

// Text as carried on an element (conf minus wrap, text arena-copied).
struct Text {
    String text;
    u16    size;
    Color  color;
};

// The element declaration. A ZII POD built with designated initializers at
// the call site; every field's zero is the natural default. Negative
// constraint values (min/max, padding, gap, border) clamp to zero at
// declaration; width/height Size values are expected non-negative.
struct ElementConf {
    ElementId id;
    Size      width;
    Size      height;
    f32       min_width;
    f32       max_width;
    f32       min_height;
    f32       max_height;
    // Per-axis ceiling as a fraction of the parent (0 = none); combines with
    // the pixel max_* — the smaller cap wins.
    V2        max_fraction;
    Direction direction;
    Align     align_x;
    Align     align_y;
    Padding   padding;
    f32       gap;
    Color     background;
    Border    border;
    TextConf  text;
    // Scrolling implies clipping (applied when the element is declared).
    // Give scroll axes a non-Fit size — Fit grows to the content, leaving
    // nothing to scroll.
    b32 clip;
    b32 scroll_x;
    b32 scroll_y;
    // Floating removes the element from its parent's flow and pins the point
    // at anchor_self on this element to the point at anchor_parent on the
    // parent's final bounds: {0,0} is the top-left, {0.5,0.5} the center,
    // {1,0} the top-right. Floating elements take no flow space, escape
    // ancestor clipping, and draw above the normal tree ordered by z_index.
    // Grow and Parent sizes resolve against the parent's outer bounds, so a
    // Grow float at the top level covers the whole viewport (a modal).
    b32 floating;
    i16 z_index;
    V2  anchor_parent;
    V2  anchor_self;
    // Extra displacement applied after the anchors are pinned.
    V2 float_offset;
    // Texture stretched over the element's bounds. source is the texture's
    // natural pixel size: it becomes the element's intrinsic size, so Fit
    // axes take the image's own dimensions while explicit sizes stretch it.
    ImageId image;
    V2      image_source;
    Color   image_tint; // multiplied over the image; the zero color means untinted
    f32     image_fade; // 0 draws fully, 1 hides entirely; blends by transparency
};

struct Sense {
    b32 clicked;
    b32 hovered;
    b32 held;
};

struct Input {
    Rect bounds;
    V2   mouse_pos;
    b32  mouse_pressed; // the primary button went down this frame
    b32  mouse_down;    // the primary button is currently held
    // Scroll delta for this frame; positive y (wheel up) scrolls back toward
    // the start of the content.
    V2 wheel;
};

// ------------------------------------------------------------- draw output

enum class DrawKind : u32 {
    Nil,
    Rectangle,
    Text,
    Border,
    // Draw the texture behind image, stretched over bounds and multiplied by
    // color.
    Image,
    // Restrict drawing to bounds (intersected with any enclosing clip) until
    // the matching ClipEnd.
    ClipStart,
    ClipEnd,
};

struct DrawCommand {
    DrawKind  kind;
    ElementId id;
    Rect      bounds;
    Color     color;
    String    text; // into the engine's frame arena; dies at the next layout call
    u16       text_size;
    f32       text_baseline;
    f32       border_width;
    f32       corner_radius;
    ImageId   image;
};

// One frame's draw output: engine-resident, recycled every frame. Command
// text points into the engine's frame arena, so the whole Output dies at
// the next layout call. duplicate_ids lists ids declared by more than one
// element this frame, once per extra occurrence — duplicates corrupt
// everything keyed on identity (sense, hover, scroll state), so treat any
// entry as a bug.
struct Output {
    vec::Vec<DrawCommand> commands;
    b32                   is_pointer_over_ui;
    vec::Vec<ElementId>   duplicate_ids;
};

// ---------------------------------------------------------- engine internals

// Persistent scroll state for one clipping element, carried across frames by
// element id like previous_bounds.
struct ScrollState {
    // How far the content is scrolled; children shift by its negation.
    V2 offset;
    // Content overhang beyond the inner bounds as of last frame; offset
    // clamps to [0, max_offset] per axis.
    V2 max_offset;
    // Clip-intersected bounds from last frame, for wheel targeting.
    Rect visible_bounds;
    b32  horizontal;
    b32  vertical;
    // Index in last frame's element tree; children sort after parents, so
    // the innermost scroll region under the pointer wins.
    u32 order;
    // Present in the current frame; stale states are pruned.
    b32 live;
};

// A cached measurement. The measured text is never stored — the key is a
// hash of (size, text) — so a 64-bit hash collision silently shares a
// measurement, as in Clay.
struct TextCacheEntry {
    // Frame of the last hit; entries not hit for a full frame are evicted at
    // the start of the next one.
    u32         generation;
    TextMetrics metrics;
};

// Persistent, Clay-style measurement cache. Keying by hash means it borrows
// nothing from the frame arena, and generational eviction bounds it to
// roughly the text measured in the last frame. heights caches per-size line
// metrics (never evicted; one entry per font size in use).
struct TextCache {
    hashtable::Hashtable<TextCacheEntry> entries;
    u32                              generation;
    hashtable::Hashtable<TextMetrics>    heights;
};

struct TextLine {
    String      text;
    TextMetrics metrics;
};

// The per-frame layout node: the conf's fields plus tree links and computed
// results. Lives in the engine's frame arena; index 0 is the root, 0 links
// mean null.
struct Element {
    ElementId id;
    usize     first_child;
    usize     last_child;
    usize     next_sibling;
    u32       child_count;
    Size      width;
    Size      height;
    f32       min_width;
    f32       max_width;
    f32       min_height;
    f32       max_height;
    V2        max_fraction;
    Direction direction;
    Align     align_x;
    Align     align_y;
    Padding   padding;
    f32       gap;
    Color     background;
    Border    border;
    Text      text;
    b32       wrap_text;
    b32       clip;
    b32       scroll_x;
    b32       scroll_y;
    V2        scroll_offset;
    V2        content_size;
    b32       floating;
    i16       z_index;
    V2        anchor_parent;
    V2        anchor_self;
    V2        float_offset;
    ImageId   image;
    V2        image_source;
    Color     image_tint;
    f32       image_fade;
    Slice<TextLine> text_lines;
    V2              measured_text;
    // What the element asks of a fit parent: content for fit and unbounded
    // growers, the cap for capped growers, pixels for fixed.
    V2 intrinsic;
    // The floor a growing element never shrinks below: unavoidable content
    // (text, image, the children's own floors), inside the explicit
    // constraints. Built bottom-up alongside intrinsic.
    V2  minimum;
    f32 resolved_main;
    Rect bounds;
};

// The persistent engine. ZII — the first layout call reserves its arenas and
// wires the internal containers; after that the struct must stay put (the
// containers point back into it), so own it by value in one place and pass
// pointers.
struct Engine {
    arena::Arena frame;      // reset at the top of every layout call
    arena::Arena persistent; // backs output + caches; never reset
    Output       output;
    hashtable::Hashtable<Rect>        previous_bounds;
    hashtable::Hashtable<Rect>        current_bounds;
    TextCache                     text_measurements;
    hashtable::Hashtable<ScrollState> scroll_states;
};

// The frame-scoped builder handed to the build callable. Declare elements
// through add / add_with; query interaction through sense.
struct Ui {
    arena::Arena*                  arena;
    vec::Vec<Element>*             nodes;
    vec::Vec<usize>*               parents;
    hashtable::Hashtable<Rect>*        previous_bounds;
    hashtable::Hashtable<ScrollState>* scroll_states;
    Input                          input;
};

// ------------------------------------------------------------------ concepts

// The measure callable: (String, u16 size) -> TextMetrics. Injected so the
// engine stays renderer-free and tests can supply deterministic metrics.
template <typename M>
concept MeasureText = requires(M measure, String text, u16 size, TextMetrics metrics) {
    metrics = measure(text, size);
};

// The frame build callable: receives the Ui to declare elements into.
template <typename B>
concept FrameBody = requires(B build, Ui* ui) { build(ui); };

// A container body callable: no arguments — the call site's Ui pointer is
// already in scope for the lambda to capture.
template <typename B>
concept ElementBody = requires(B body) { body(); };

// --------------------------------------------------------------- declaration

// The element's interaction state, judged against last frame's clipped
// bounds (one frame of latency). Queryable before the element is declared,
// which is what styling needs — the Sense returned by add arrives after the
// element's looks are already committed:
//
//     b32 hovered = ui::layout::sense(u, "button").hovered;
//     ui::layout::add(u, {.id = "button", .background = hovered ? HI : LO});
fn Sense sense(Ui* ui, ElementId id) {
    Rect* bounds  = hashtable::get(ui->previous_bounds, id.value);
    b32   hovered = bounds && contains(*bounds, ui->input.mouse_pos);
    return {
        .clicked = hovered && ui->input.mouse_pressed,
        .hovered = hovered,
        .held    = hovered && ui->input.mouse_down,
    };
}

struct PushResult {
    usize index;
    Sense sense;
};

fn PushResult push_element(Ui* ui, ElementConf conf) {
    usize parent  = (*ui->parents)[ui->parents->len - 1];
    u32   ordinal = (*ui->nodes)[parent].child_count;
    ElementId id  = conf.id.value == 0 ? ElementId{id_child((*ui->nodes)[parent].id.value, ordinal)} : conf.id;
    Sense sensed  = sense(ui, id);

    b32 clip          = conf.clip || conf.scroll_x || conf.scroll_y;
    V2  scroll_offset = {};
    if (clip) {
        ScrollState* state = hashtable::get(ui->scroll_states, id.value);
        if (state) scroll_offset = state->offset;
    }
    String text = {};
    if (conf.text.text.len) { text = arena::clone_string(ui->arena, conf.text.text); }

    // Negative sizing values mean zero (the Rust builder clamped in its
    // setters; designated init clamps here, at the declaration boundary).
    Element element       = {};
    element.id            = id;
    element.width         = conf.width;
    element.height        = conf.height;
    element.min_width     = max(conf.min_width, 0.0f);
    element.max_width     = max(conf.max_width, 0.0f);
    element.min_height    = max(conf.min_height, 0.0f);
    element.max_height    = max(conf.max_height, 0.0f);
    element.max_fraction  = {max(conf.max_fraction.x, 0.0f), max(conf.max_fraction.y, 0.0f)};
    element.direction     = conf.direction;
    element.align_x       = conf.align_x;
    element.align_y       = conf.align_y;
    element.padding       = {max(conf.padding.left, 0.0f), max(conf.padding.right, 0.0f),
                             max(conf.padding.top, 0.0f), max(conf.padding.bottom, 0.0f)};
    element.gap           = max(conf.gap, 0.0f);
    element.background    = conf.background;
    element.border        = {max(conf.border.width, 0.0f), conf.border.color, max(conf.border.radius, 0.0f)};
    element.text          = {text, conf.text.size, conf.text.color};
    element.wrap_text     = conf.text.wrap;
    element.clip          = clip;
    element.scroll_x      = conf.scroll_x;
    element.scroll_y      = conf.scroll_y;
    element.scroll_offset = scroll_offset;
    element.floating      = conf.floating;
    element.z_index       = conf.z_index;
    element.anchor_parent = conf.anchor_parent;
    element.anchor_self   = conf.anchor_self;
    element.float_offset  = conf.float_offset;
    element.image         = conf.image;
    element.image_source  = conf.image_source;
    element.image_tint    = conf.image_tint;
    element.image_fade    = clamp(conf.image_fade, 0.0f, 1.0f);

    usize index = ui->nodes->len;
    vec::push(ui->nodes, element);

    Element* parent_node = &(*ui->nodes)[parent];
    usize    previous    = parent_node->last_child;
    if (previous == 0) {
        parent_node->first_child = index;
    } else {
        (*ui->nodes)[previous].next_sibling = index;
    }
    parent_node->last_child = index;
    parent_node->child_count += 1;
    return {index, sensed};
}

fn Sense add(Ui* ui, ElementConf conf) { return push_element(ui, conf).sense; }

template <ElementBody B> fn Sense add_with(Ui* ui, ElementConf conf, B body) {
    PushResult result = push_element(ui, conf);
    vec::push(ui->parents, result.index);
    body();
    vec::pop(ui->parents);
    return result.sense;
}

// ------------------------------------------------------------ text measuring

fn u64 hash_text(String text, u16 size) {
    u64 hash = 0xcbf29ce484222325ull;
    hash ^= (u8)(size & 0xff);
    hash *= 0x100000001b3ull;
    hash ^= (u8)(size >> 8);
    hash *= 0x100000001b3ull;
    hash = fnv1a(text, hash);
    return max(hash, (u64)1); // key 0 is the table's empty marker
}

// Fixed string spanning a font's full vertical range (tall accented cap,
// deep descenders). Line height and baseline come from measuring this probe,
// so a text's box depends only on its size — never on which glyphs the
// string happens to contain.
inline constexpr const char* LINE_PROBE = "ÁM|jgqp";

template <MeasureText M> fn TextMetrics cache_measure(TextCache* cache, M& measure_text, String text, u16 size) {
    u64             hash  = hash_text(text, size);
    TextCacheEntry* entry = hashtable::get(&cache->entries, hash);
    if (entry) {
        entry->generation = cache->generation;
        return entry->metrics;
    }
    TextMetrics metrics = measure_text(text, size);
    // The height does not depend on the actual text, only on the size; both
    // come from the probe.
    u64          height_key = (u64)size + 0x100000000ull; // offset clear of the 0 empty marker
    TextMetrics* probe      = hashtable::get(&cache->heights, height_key);
    if (!probe) {
        TextMetrics probed = measure_text(String(LINE_PROBE), size);
        probe              = hashtable::put(&cache->heights, height_key);
        *probe             = probed;
    }
    metrics.size.y   = probe->size.y;
    metrics.baseline = probe->baseline;
    entry            = hashtable::put(&cache->entries, hash);
    *entry           = {cache->generation, metrics};
    return metrics;
}

fn void cache_begin_frame(TextCache* cache, arena::Arena* scratch_arena) {
    u32 previous = cache->generation;
    cache->generation += 1;
    arena::ScratchArena scratch(scratch_arena);
    vec::Vec<u64>       dead = vec::make_vec<u64>(scratch_arena, cache->entries.count);
    for (auto entry : cache->entries) {
        if (entry.value->generation != previous) vec::push(&dead, entry.key);
    }
    for (u64 key : dead) hashtable::remove(&cache->entries, key);
}

// UTF-8: byte length of the character starting at this byte. Invalid leading
// bytes advance one byte so scanning stays finite.
fn usize utf8_len(char c) {
    u8 byte = (u8)c;
    if (byte < 0x80) return 1;
    if ((byte >> 5) == 0x06) return 2;
    if ((byte >> 4) == 0x0e) return 3;
    if ((byte >> 3) == 0x1e) return 4;
    return 1;
}

fn b32 is_space_byte(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

fn String substr(String s, usize start, usize end) { return {end - start, s.data + start}; }

fn String trim_end(String s) {
    while (s.len && is_space_byte(s.data[s.len - 1])) s.len -= 1;
    return s;
}

template <MeasureText M>
fn void push_text_line(vec::Vec<TextLine>* lines, String line, Text text, TextCache* cache, M& measure_text,
                       V2* measured) {
    // An empty line still takes a line's height; the space probe supplies it
    // while the width stays zero.
    TextMetrics metrics = cache_measure(cache, measure_text, line.len ? line : String(" "), text.size);
    if (!line.len) metrics.size.x = 0;
    measured->x = max(measured->x, metrics.size.x);
    measured->y += metrics.size.y;
    vec::push(lines, TextLine{line, metrics});
}

struct BreakResult {
    Slice<TextLine> lines;
    V2              measured;
};

// Greedy word wrap: lines break at the last whitespace that fits, falling
// back to a hard character break when a single word overflows. An infinite
// max_width means no wrapping — split on newlines only.
template <MeasureText M>
fn BreakResult break_text_lines(arena::Arena* arena, Text text, f32 max_width, TextCache* cache, M& measure_text) {
    vec::Vec<TextLine> lines    = vec::make_vec<TextLine>(arena, 4);
    V2                 measured = {};
    if (isinf(max_width)) {
        usize start = 0;
        for (usize i = 0; i <= text.text.len; ++i) {
            if (i == text.text.len || text.text.data[i] == '\n') {
                push_text_line(&lines, substr(text.text, start, i), text, cache, measure_text, &measured);
                start = i + 1;
            }
        }
        return {vec::slice(lines), measured};
    }

    usize line_start = 0;
    while (true) {
        if (line_start == text.text.len) {
            if (text.text.len && text.text.data[text.text.len - 1] == '\n') {
                push_text_line(&lines, String{}, text, cache, measure_text, &measured);
            }
            break;
        }

        usize cursor     = line_start;
        usize best_end   = line_start;
        usize last_break = 0;
        b32   has_break  = false;
        b32   emitted    = false;
        while (cursor < text.text.len) {
            char  character = text.text.data[cursor];
            usize next      = min(cursor + utf8_len(character), text.text.len);
            if (character == '\n') {
                push_text_line(&lines, trim_end(substr(text.text, line_start, cursor)), text, cache, measure_text,
                               &measured);
                line_start = next;
                emitted    = true;
                break;
            }

            String      candidate         = trim_end(substr(text.text, line_start, next));
            TextMetrics candidate_metrics = cache_measure(cache, measure_text, candidate, text.size);
            if (candidate_metrics.size.x > max_width && best_end > line_start) {
                usize line_end = (has_break && last_break > line_start) ? last_break : best_end;
                push_text_line(&lines, trim_end(substr(text.text, line_start, line_end)), text, cache, measure_text,
                               &measured);
                line_start = line_end;
                while (line_start < text.text.len) {
                    char leading = text.text.data[line_start];
                    if (leading == '\n' || !is_space_byte(leading)) break;
                    line_start += utf8_len(leading);
                }
                emitted = true;
                break;
            }
            if (candidate_metrics.size.x > max_width) {
                // A single overflowing word: hard break after this character.
                push_text_line(&lines, substr(text.text, line_start, next), text, cache, measure_text, &measured);
                line_start = next;
                emitted    = true;
                break;
            }

            if (is_space_byte(character)) {
                last_break = cursor;
                has_break  = true;
            }
            best_end = next;
            cursor   = next;
        }

        if (emitted) continue;
        push_text_line(&lines, trim_end(substr(text.text, line_start, text.text.len)), text, cache, measure_text,
                       &measured);
        break;
    }

    return {vec::slice(lines), measured};
}

template <MeasureText M>
fn void measure_text_elements(arena::Arena* arena, Slice<Element> nodes, TextCache* cache, M& measure_text, b32 wrap) {
    for (usize index = 0; index < nodes.len; ++index) {
        Element node = nodes[index];
        if (wrap && !node.wrap_text) continue;
        if (node.text.text.len == 0) {
            nodes[index].text_lines    = {};
            nodes[index].measured_text = {};
            continue;
        }
        f32 max_width = (wrap && node.wrap_text)
                            ? max(node.bounds.w - node.padding.left - node.padding.right, 0.0f)
                            : (f32)INFINITY;
        BreakResult result         = break_text_lines(arena, node.text, max_width, cache, measure_text);
        nodes[index].text_lines    = result.lines;
        nodes[index].measured_text = result.measured;
    }
}

// ------------------------------------------------------------------ sizing

fn f32 effective_max(f32 min_value, f32 max_value) {
    return max_value == 0.0f ? (f32)INFINITY : max(max_value, min_value);
}

fn f32 constrain_axis(f32 value, f32 min_value, f32 max_value) {
    return min(max(value, min_value), effective_max(min_value, max_value));
}

// Two ceilings, 0 = none each; the smaller real one wins.
fn f32 combine_caps(f32 a, f32 b) {
    if (a > 0 && b > 0) return min(a, b);
    if (a > 0) return a;
    if (b > 0) return b;
    return 0;
}

struct AxisConstraints {
    f32 floor;
    f32 cap;
};

// The (min, max) pair for one axis, with the fractional cap resolved against
// the parent space this element is being laid out in.
fn AxisConstraints axis_constraints(const Element* node, b32 horizontal, f32 available) {
    f32 min_value = horizontal ? node->min_width : node->min_height;
    f32 max_value = horizontal ? node->max_width : node->max_height;
    f32 fraction  = horizontal ? node->max_fraction.x : node->max_fraction.y;
    f32 fraction_cap = fraction > 0 ? fraction * max(available, 0.0f) : 0.0f;
    return {min_value, combine_caps(max_value, fraction_cap)};
}

// The floor a growing element never shrinks below: its bottom-up minimum,
// kept under the (possibly fraction-resolved) cap — an explicit cap is a
// hard ceiling even against content.
fn f32 grow_floor(const Element* node, b32 horizontal, f32 min_value, f32 max_value) {
    f32 minimum = horizontal ? node->minimum.x : node->minimum.y;
    return min(max(minimum, min_value), effective_max(0.0f, max_value));
}

fn f32 resolve_axis(Size size, f32 parent, f32 intrinsic) {
    switch (size.mode) {
        case SizeMode::Fit: return intrinsic;
        case SizeMode::Grow: return parent;
        case SizeMode::Pixels: return max(size.value, 0.0f);
        case SizeMode::Parent: return parent * clamp(size.value, 0.0f, 1.0f);
    }
    return intrinsic;
}

// What the element asks of a fit parent. A capped grower asks for its cap —
// it intends to grow into it — so fixed-looking sizes reserve their pixels
// inside fit containers. Fractional caps can't be asked for here (the parent
// isn't sized yet), so those growers ask for their content.
fn f32 intrinsic_axis(Size size, f32 natural, f32 max_value) {
    switch (size.mode) {
        case SizeMode::Pixels: return max(size.value, 0.0f);
        case SizeMode::Parent: return 0;
        case SizeMode::Fit: return natural;
        case SizeMode::Grow: return max_value > 0 ? max(natural, max_value) : natural;
    }
    return natural;
}

// The floor only tracks content for growing axes: fixed (pixel / fraction)
// axes keep their deliberate compress-to-explicit-min behavior, and fit axes
// already are their content.
fn f32 floor_axis(Size size, f32 unavoidable, f32 min_value, f32 max_value) {
    switch (size.mode) {
        case SizeMode::Grow:
        case SizeMode::Fit: return constrain_axis(unavoidable, min_value, max(max_value, 0.0f));
        case SizeMode::Pixels:
        case SizeMode::Parent: return min_value;
    }
    return min_value;
}

fn f32 align_offset(Align align, f32 available, f32 occupied) {
    f32 free = max(available - occupied, 0.0f);
    switch (align) {
        case Align::Start: return 0;
        case Align::Center: return free * 0.5f;
        case Align::End: return free;
    }
    return 0;
}

// Text, image source size, and children all compete for the element's
// natural content size.
fn V2 pad_content(const Element* node, V2 content) {
    return {
        max(max(max(node->measured_text.x, node->image_source.x), content.x) + node->padding.left +
                node->padding.right,
            0.0f),
        max(max(max(node->measured_text.y, node->image_source.y), content.y) + node->padding.top +
                node->padding.bottom,
            0.0f),
    };
}

// Bottom-up (children have higher indices than parents): computes every
// element's intrinsic size and minimum floor from its content and children.
fn void measure_elements(Slice<Element> nodes) {
    for (usize index = nodes.len; index-- > 0;) {
        Element node = nodes[index];

        usize child       = node.first_child;
        usize child_count = 0;
        f32   main        = 0;
        f32   cross       = 0;
        f32   main_floor  = 0;
        f32   cross_floor = 0;
        while (child != 0) {
            // Floating children take no flow space.
            if (nodes[child].floating) {
                child = nodes[child].next_sibling;
                continue;
            }
            V2 size  = nodes[child].intrinsic;
            V2 floor = nodes[child].minimum;
            if (node.direction == Direction::LeftToRight) {
                main += size.x;
                cross = max(cross, size.y);
                main_floor += floor.x;
                cross_floor = max(cross_floor, floor.y);
            } else {
                main += size.y;
                cross = max(cross, size.x);
                main_floor += floor.y;
                cross_floor = max(cross_floor, floor.x);
            }
            child_count += 1;
            child = nodes[child].next_sibling;
        }
        if (child_count > 1) {
            f32 gaps = node.gap * (f32)(child_count - 1);
            main += gaps;
            main_floor += gaps;
        }

        b32 horizontal    = node.direction == Direction::LeftToRight;
        V2  content       = horizontal ? V2{main, cross} : V2{cross, main};
        V2  floor_content = horizontal ? V2{main_floor, cross_floor} : V2{cross_floor, main_floor};

        V2 natural     = pad_content(&node, content);
        V2 unavoidable = pad_content(&node, floor_content);
        nodes[index].minimum = {
            floor_axis(node.width, unavoidable.x, node.min_width, node.max_width),
            floor_axis(node.height, unavoidable.y, node.min_height, node.max_height),
        };
        nodes[index].intrinsic = {
            constrain_axis(intrinsic_axis(node.width, natural.x, node.max_width), node.min_width, node.max_width),
            constrain_axis(intrinsic_axis(node.height, natural.y, node.max_height), node.min_height, node.max_height),
        };
    }
}

// --------------------------------------------------------------- arranging

// Top-down placement of one parent's children: resolve main-axis sizes
// (compressing overflow and distributing grow space, each via a 32-iteration
// binary search), place along the flow with gap and alignment, then pin
// floating children against the parent's final bounds.
fn void arrange_children(usize parent_index, Slice<Element> nodes) {
    Element parent = nodes[parent_index];
    if (parent.first_child == 0) return;

    // Scroll offsets shift where children are placed, not how much room they
    // are given.
    Rect inner = {
        parent.bounds.x + parent.padding.left - parent.scroll_offset.x,
        parent.bounds.y + parent.padding.top - parent.scroll_offset.y,
        max(parent.bounds.w - parent.padding.left - parent.padding.right, 0.0f),
        max(parent.bounds.h - parent.padding.top - parent.padding.bottom, 0.0f),
    };
    b32 horizontal      = parent.direction == Direction::LeftToRight;
    f32 available_main  = horizontal ? inner.w : inner.h;
    f32 available_cross = horizontal ? inner.h : inner.w;

    usize child                   = parent.first_child;
    u32   flow_count              = 0;
    f32   fixed_main              = 0;
    f32   fixed_minimum           = 0;
    b32   has_grow                = false;
    f32   grow_minimum            = 0;
    f32   minimum_positive_weight = (f32)INFINITY;
    while (child != 0) {
        Element node = nodes[child];
        if (node.floating) {
            child = node.next_sibling;
            continue;
        }
        flow_count += 1;
        Size            size = horizontal ? node.width : node.height;
        AxisConstraints c    = axis_constraints(&node, horizontal, available_main);
        if (is_grow(size)) {
            f32 weight = grow_weight(size);
            has_grow   = true;
            grow_minimum += grow_floor(&node, horizontal, c.floor, c.cap);
            if (weight > 0) minimum_positive_weight = min(minimum_positive_weight, weight);
        } else {
            f32 resolved = constrain_axis(
                resolve_axis(size, available_main, horizontal ? node.intrinsic.x : node.intrinsic.y), c.floor, c.cap);
            nodes[child].resolved_main = resolved;
            fixed_main += resolved;
            fixed_minimum += c.floor;
        }
        child = node.next_sibling;
    }
    f32 gap_total = parent.gap * (f32)(flow_count ? flow_count - 1 : 0);

    b32 scrolls_main = horizontal ? parent.scroll_x : parent.scroll_y;
    // A scrolling main axis lets content overflow instead of compressing it.
    if (!scrolls_main && fixed_main + grow_minimum + gap_total > available_main && fixed_main > fixed_minimum) {
        f32 fixed_target = max(available_main - grow_minimum - gap_total, fixed_minimum);
        f32 low_scale    = 0;
        f32 high_scale   = 1;
        if (fixed_target > fixed_minimum) {
            for (u32 iteration = 0; iteration < 32; ++iteration) {
                f32 scale = (low_scale + high_scale) * 0.5f;
                f32 total = 0;
                child     = parent.first_child;
                while (child != 0) {
                    Element node = nodes[child];
                    Size    size = horizontal ? node.width : node.height;
                    if (!node.floating && !is_grow(size)) {
                        AxisConstraints c = axis_constraints(&node, horizontal, available_main);
                        total += max(node.resolved_main * scale, c.floor);
                    }
                    child = node.next_sibling;
                }
                if (total < fixed_target) {
                    low_scale = scale;
                } else {
                    high_scale = scale;
                }
            }
        } else {
            high_scale = 0;
        }

        fixed_main = 0;
        child      = parent.first_child;
        while (child != 0) {
            Element node = nodes[child];
            Size    size = horizontal ? node.width : node.height;
            if (!node.floating && !is_grow(size)) {
                AxisConstraints c          = axis_constraints(&node, horizontal, available_main);
                nodes[child].resolved_main = max(node.resolved_main * high_scale, c.floor);
                fixed_main += nodes[child].resolved_main;
            }
            child = node.next_sibling;
        }
    }

    if (has_grow) {
        f32 grow_target = max(available_main - fixed_main - gap_total, grow_minimum);
        f32 low_scale   = 0;
        f32 high_scale  = isfinite(minimum_positive_weight) ? grow_target / minimum_positive_weight : 0.0f;
        for (u32 iteration = 0; iteration < 32; ++iteration) {
            f32 scale = (low_scale + high_scale) * 0.5f;
            f32 total = 0;
            child     = parent.first_child;
            while (child != 0) {
                Element node = nodes[child];
                Size    size = horizontal ? node.width : node.height;
                if (!node.floating && is_grow(size)) {
                    f32             weight = grow_weight(size);
                    AxisConstraints c      = axis_constraints(&node, horizontal, available_main);
                    f32             floor  = grow_floor(&node, horizontal, c.floor, c.cap);
                    total += constrain_axis(weight * scale, floor, c.cap);
                }
                child = node.next_sibling;
            }
            if (total < grow_target) {
                low_scale = scale;
            } else {
                high_scale = scale;
            }
        }

        child = parent.first_child;
        while (child != 0) {
            Element node = nodes[child];
            Size    size = horizontal ? node.width : node.height;
            if (is_grow(size)) {
                f32             weight     = grow_weight(size);
                AxisConstraints c          = axis_constraints(&node, horizontal, available_main);
                f32             floor      = grow_floor(&node, horizontal, c.floor, c.cap);
                nodes[child].resolved_main = constrain_axis(weight * high_scale, floor, c.cap);
            }
            child = node.next_sibling;
        }
    }

    f32 occupied = fixed_main + gap_total;
    child        = parent.first_child;
    while (child != 0) {
        if (!nodes[child].floating && is_grow(horizontal ? nodes[child].width : nodes[child].height)) {
            occupied += nodes[child].resolved_main;
        }
        child = nodes[child].next_sibling;
    }
    Align main_align    = horizontal ? parent.align_x : parent.align_y;
    f32   cursor        = align_offset(main_align, available_main, occupied);
    f32   content_cross = 0;

    child = parent.first_child;
    while (child != 0) {
        Element node = nodes[child];
        if (node.floating) {
            child = node.next_sibling;
            continue;
        }
        Size            cross_size_mode = horizontal ? node.height : node.width;
        f32             cross_intrinsic = horizontal ? node.intrinsic.y : node.intrinsic.x;
        AxisConstraints c               = axis_constraints(&node, !horizontal, available_cross);
        f32 cross_floor = is_grow(cross_size_mode) ? grow_floor(&node, !horizontal, c.floor, c.cap) : c.floor;
        f32 main_size   = node.resolved_main;
        f32 cross_size =
            constrain_axis(resolve_axis(cross_size_mode, available_cross, cross_intrinsic), cross_floor, c.cap);
        Align cross_align = horizontal ? parent.align_y : parent.align_x;
        content_cross     = max(content_cross, cross_size);
        f32 cross         = align_offset(cross_align, available_cross, cross_size);

        nodes[child].bounds = horizontal ? Rect{inner.x + cursor, inner.y + cross, main_size, cross_size}
                                         : Rect{inner.x + cross, inner.y + cursor, cross_size, main_size};
        arrange_children(child, nodes);
        cursor += main_size + parent.gap;
        child = node.next_sibling;
    }

    // Floating children are placed after flow layout, against the parent's
    // final bounds: their own anchor point lands on the parent's, plus the
    // configured offset. Sizes resolve against the parent's outer bounds.
    child = parent.first_child;
    while (child != 0) {
        Element node = nodes[child];
        if (!node.floating) {
            child = node.next_sibling;
            continue;
        }
        AxisConstraints cx      = axis_constraints(&node, true, parent.bounds.w);
        AxisConstraints cy      = axis_constraints(&node, false, parent.bounds.h);
        f32             floor_x = is_grow(node.width) ? grow_floor(&node, true, cx.floor, cx.cap) : cx.floor;
        f32             floor_y = is_grow(node.height) ? grow_floor(&node, false, cy.floor, cy.cap) : cy.floor;
        V2              size    = {
            constrain_axis(resolve_axis(node.width, parent.bounds.w, node.intrinsic.x), floor_x, cx.cap),
            constrain_axis(resolve_axis(node.height, parent.bounds.h, node.intrinsic.y), floor_y, cy.cap),
        };
        V2 target = {
            parent.bounds.x + parent.bounds.w * node.anchor_parent.x,
            parent.bounds.y + parent.bounds.h * node.anchor_parent.y,
        };
        nodes[child].bounds = {
            target.x + node.float_offset.x - size.x * node.anchor_self.x,
            target.y + node.float_offset.y - size.y * node.anchor_self.y,
            size.x,
            size.y,
        };
        arrange_children(child, nodes);
        child = node.next_sibling;
    }

    nodes[parent_index].content_size = horizontal ? V2{occupied, content_cross} : V2{content_cross, occupied};
}

// -------------------------------------------------------- output & sensing

// The clip state of elements outside any clipping container: a rectangle so
// large that intersecting with it changes nothing.
inline constexpr f32  F32_HUGE  = 3.402823466e+38f;
inline constexpr Rect UNCLIPPED = {-F32_HUGE / 2, -F32_HUGE / 2, F32_HUGE, F32_HUGE};

// Records every element's on-screen bounds for next frame's Sense and wheel
// targeting, shrunk to what clipping actually leaves visible. An insert that
// displaces a previous entry means two elements share an id; each extra
// occurrence is reported as a duplicate.
fn void record_bounds(usize parent, Rect clip, Slice<Element> nodes, hashtable::Hashtable<Rect>* bounds,
                      vec::Vec<ElementId>* duplicates) {
    usize child = nodes[parent].first_child;
    while (child != 0) {
        Element node = nodes[child];
        // Floating elements escape ancestor clipping.
        Rect effective_clip = node.floating ? UNCLIPPED : clip;
        Rect visible        = intersect(node.bounds, effective_clip);
        if (hashtable::get(bounds, node.id.value)) vec::push(duplicates, node.id);
        *hashtable::put(bounds, node.id.value) = visible;
        record_bounds(child, node.clip ? visible : effective_clip, nodes, bounds, duplicates);
        child = node.next_sibling;
    }
}

fn void emit_element(usize index, Slice<Element> nodes, Output* out);

fn void emit_children(usize parent, Slice<Element> nodes, Output* out) {
    usize child = nodes[parent].first_child;
    while (child != 0) {
        // Floating subtrees are emitted in a separate z-ordered pass.
        if (!nodes[child].floating) emit_element(child, nodes, out);
        child = nodes[child].next_sibling;
    }
}

fn void emit_element(usize index, Slice<Element> nodes, Output* out) {
    Element node = nodes[index];
    if (node.background.a > 0) {
        vec::push(&out->commands, DrawCommand{
                                      .kind          = DrawKind::Rectangle,
                                      .id            = node.id,
                                      .bounds        = node.bounds,
                                      .color         = node.background,
                                      .corner_radius = node.border.radius,
                                  });
    }
    if (node.image.value != 0 && node.image_fade < 1.0f) {
        // The zero tint means untinted (ZII), so the renderer can always
        // multiply by the command color. Fade rides the resolved alpha — the
        // image blends toward whatever draws beneath it — and a fully faded
        // image is skipped outright.
        Color tint = node.image_tint.a > 0 ? node.image_tint : Color{255, 255, 255, 255};
        tint.a     = (u8)((f32)tint.a * (1.0f - node.image_fade));
        vec::push(&out->commands, DrawCommand{
                                      .kind   = DrawKind::Image,
                                      .id     = node.id,
                                      .bounds = node.bounds,
                                      .color  = tint,
                                      .image  = node.image,
                                  });
    }
    if (node.clip) {
        vec::push(&out->commands, DrawCommand{.kind = DrawKind::ClipStart, .id = node.id, .bounds = node.bounds});
    }
    if (node.text.text.len) {
        Rect text_bounds = {
            node.bounds.x + node.padding.left,
            node.bounds.y + node.padding.top,
            max(node.bounds.w - node.padding.left - node.padding.right, 0.0f),
            max(node.bounds.h - node.padding.top - node.padding.bottom, 0.0f),
        };
        f32 y = text_bounds.y;
        for (const TextLine& line : node.text_lines) {
            if (line.text.len) {
                // The line text lives in the frame arena (the element's own
                // arena copy), so it stays valid until the next layout call.
                vec::push(&out->commands, DrawCommand{
                                              .kind          = DrawKind::Text,
                                              .id            = node.id,
                                              .bounds        = {text_bounds.x, y, text_bounds.w, line.metrics.size.y},
                                              .color         = node.text.color,
                                              .text          = line.text,
                                              .text_size     = node.text.size,
                                              .text_baseline = line.metrics.baseline,
                                          });
            }
            y += line.metrics.size.y;
        }
    }
    emit_children(index, nodes, out);
    if (node.clip) {
        vec::push(&out->commands, DrawCommand{.kind = DrawKind::ClipEnd, .id = node.id, .bounds = node.bounds});
    }
    // The border sits on the element's own edge, outside its clip region.
    if (node.border.width > 0 && node.border.color.a > 0) {
        vec::push(&out->commands, DrawCommand{
                                      .kind          = DrawKind::Border,
                                      .id            = node.id,
                                      .bounds        = node.bounds,
                                      .color         = node.border.color,
                                      .border_width  = node.border.width,
                                      .corner_radius = node.border.radius,
                                  });
    }
}

fn void apply_wheel(hashtable::Hashtable<ScrollState>* scroll_states, Input input) {
    if (input.wheel.x == 0 && input.wheel.y == 0) return;
    // The innermost scroll region under the pointer wins (max order —
    // children sort after parents in the element tree).
    ScrollState* target = 0;
    for (auto entry : *scroll_states) {
        ScrollState* state = entry.value;
        if (!(state->horizontal || state->vertical)) continue;
        if (!contains(state->visible_bounds, input.mouse_pos)) continue;
        if (!target || state->order > target->order) target = state;
    }
    if (!target) return;
    if (target->horizontal) target->offset.x = clamp(target->offset.x - input.wheel.x, 0.0f, target->max_offset.x);
    if (target->vertical) target->offset.y = clamp(target->offset.y - input.wheel.y, 0.0f, target->max_offset.y);
}

struct FloatingEntry {
    i16 z;
    u32 index;
};

// Sort by (z, declaration order). Counts are tiny; insertion sort.
fn void sort_floating(Slice<FloatingEntry> entries) {
    for (usize i = 1; i < entries.len; ++i) {
        FloatingEntry entry = entries[i];
        usize         j     = i;
        while (j > 0 &&
               (entries[j - 1].z > entry.z || (entries[j - 1].z == entry.z && entries[j - 1].index > entry.index))) {
            entries[j] = entries[j - 1];
            j -= 1;
        }
        entries[j] = entry;
    }
}

// ------------------------------------------------------------------- engine

namespace {
constexpr usize ENGINE_FRAME_ARENA_SIZE      = 64 * 1024 * 1024;
constexpr usize ENGINE_PERSISTENT_ARENA_SIZE = 64 * 1024 * 1024;
} // namespace

fn void engine_ensure(Engine* engine) {
    if (engine->persistent.base) return;
    arena::reserve(&engine->persistent, ENGINE_PERSISTENT_ARENA_SIZE);
    arena::reserve(&engine->frame, ENGINE_FRAME_ARENA_SIZE);
    arena::Arena* persistent               = &engine->persistent;
    engine->output.commands.arena          = persistent;
    engine->output.duplicate_ids.arena     = persistent;
    engine->previous_bounds.arena          = persistent;
    engine->current_bounds.arena           = persistent;
    engine->scroll_states.arena            = persistent;
    engine->text_measurements.entries.arena = persistent;
    engine->text_measurements.heights.arena = persistent;
}

// One frame: declare the tree inside build, get back the draw commands.
// Runs the measure/arrange pair twice so wrapped text (whose height depends
// on its resolved width) settles. The returned Output lives until the next
// layout call on this engine.
template <MeasureText M, FrameBody B> fn Output* layout(Engine* engine, Input input, M measure_text, B build) {
    engine_ensure(engine);
    arena::reset(&engine->frame);
    vec::clear(&engine->output.commands);
    vec::clear(&engine->output.duplicate_ids);
    engine->output.is_pointer_over_ui = false;
    hashtable::clear(&engine->current_bounds);
    cache_begin_frame(&engine->text_measurements, &engine->frame);
    apply_wheel(&engine->scroll_states, input);

    vec::Vec<Element> node_vec = vec::make_vec<Element>(&engine->frame, 256);
    Element           root     = {};
    root.id                    = ROOT;
    root.width                 = grow();
    root.height                = grow();
    root.direction             = Direction::TopToBottom;
    root.bounds                = input.bounds;
    vec::push(&node_vec, root);

    vec::Vec<usize> parent_stack = vec::make_vec<usize>(&engine->frame, 32);
    vec::push(&parent_stack, (usize)0);
    {
        Ui ui = {
            .arena           = &engine->frame,
            .nodes           = &node_vec,
            .parents         = &parent_stack,
            .previous_bounds = &engine->previous_bounds,
            .scroll_states   = &engine->scroll_states,
            .input           = input,
        };
        build(&ui);
    }
    Slice<Element> nodes = vec::slice(node_vec);
    Output*        out   = &engine->output;

    measure_text_elements(&engine->frame, nodes, &engine->text_measurements, measure_text, false);
    measure_elements(nodes);
    nodes[0].bounds = input.bounds;
    arrange_children(0, nodes);
    measure_text_elements(&engine->frame, nodes, &engine->text_measurements, measure_text, true);
    measure_elements(nodes);
    nodes[0].bounds = input.bounds;
    arrange_children(0, nodes);

    record_bounds(0, UNCLIPPED, nodes, &engine->current_bounds, &out->duplicate_ids);

    for (auto entry : engine->scroll_states) entry.value->live = false;
    for (usize index = 0; index < nodes.len; ++index) {
        Element node = nodes[index];
        if (!node.clip) continue;
        ScrollState* state = hashtable::put(&engine->scroll_states, node.id.value);
        state->max_offset  = {
            max(node.content_size.x - max(node.bounds.w - node.padding.left - node.padding.right, 0.0f), 0.0f),
            max(node.content_size.y - max(node.bounds.h - node.padding.top - node.padding.bottom, 0.0f), 0.0f),
        };
        state->offset.x       = clamp(state->offset.x, 0.0f, state->max_offset.x);
        state->offset.y       = clamp(state->offset.y, 0.0f, state->max_offset.y);
        state->horizontal     = node.scroll_x;
        state->vertical       = node.scroll_y;
        state->order          = (u32)index;
        Rect* visible         = hashtable::get(&engine->current_bounds, node.id.value);
        state->visible_bounds = visible ? *visible : node.bounds;
        state->live           = true;
    }
    {
        arena::ScratchArena scratch(&engine->frame);
        vec::Vec<u64>       dead = vec::make_vec<u64>(&engine->frame, 8);
        for (auto entry : engine->scroll_states) {
            if (!entry.value->live) vec::push(&dead, entry.key);
        }
        for (u64 key : dead) hashtable::remove(&engine->scroll_states, key);
    }

    emit_children(0, nodes, out);
    // Floating subtrees draw above the normal tree, lowest z first; equal z
    // keeps declaration order via the node index.
    vec::Vec<FloatingEntry> floating = vec::make_vec<FloatingEntry>(&engine->frame, 8);
    for (usize index = 0; index < nodes.len; ++index) {
        if (nodes[index].floating) vec::push(&floating, FloatingEntry{nodes[index].z_index, (u32)index});
    }
    sort_floating(vec::slice(floating));
    for (const FloatingEntry& entry : vec::slice(floating)) {
        emit_element(entry.index, nodes, out);
    }

    // Invisible containers are layout scaffolding and should not capture
    // map/game input. A visible render primitive defines the UI surface,
    // shrunk to what enclosing clip regions leave visible.
    vec::Vec<Rect> clip_stack         = vec::make_vec<Rect>(&engine->frame, 8);
    b32            is_pointer_over_ui = false;
    for (const DrawCommand& command : out->commands) {
        Rect clip = clip_stack.len ? clip_stack[clip_stack.len - 1] : UNCLIPPED;
        switch (command.kind) {
            case DrawKind::ClipStart: vec::push(&clip_stack, intersect(clip, command.bounds)); break;
            case DrawKind::ClipEnd:
                if (clip_stack.len) vec::pop(&clip_stack);
                break;
            default:
                is_pointer_over_ui =
                    is_pointer_over_ui || contains(intersect(command.bounds, clip), input.mouse_pos);
                break;
        }
    }
    out->is_pointer_over_ui = is_pointer_over_ui;

    hashtable::Hashtable<Rect> swapped = engine->previous_bounds;
    engine->previous_bounds        = engine->current_bounds;
    engine->current_bounds         = swapped;
    return out;
}

} // namespace layout
} // namespace ui
