#pragma once
#include "../core.hpp"
#include "../arena.hpp"
#include "../math.hpp"
#include "../string.hpp"
#include "../vec.hpp"
#include "../tabula.hpp"
#include "layout.hpp"
#include "style.hpp"

// Compiled UI descriptions: tabula source + style → flat IR, once per
// (re)load.
//
// The layout engine rebuilds every frame, so whatever describes the UI is
// walked every frame too. The raw tabula tree is the wrong shape for that
// (string key matching, yes/no atoms, number re-parsing), so compile
// translates it once into UiNodes: a flat array of fat structs with every
// field pre-parsed, every $VAR string pre-tokenized, and every style value
// baked in.
//
// There is no node "kind": widget names (panel, label, button, ...) are
// compiler vocabulary only — each one is a set of legal keys plus defaults
// written into the fields, and every policy decision (what may float, what a
// button looks like) is made here, once. The per-frame walk in ui::run is
// kind-blind: it applies every field of every node, binding against
// ui::data.
//
// The module is arena-backed: compile into an arena the caller owns, and
// reload by resetting that arena and compiling again. Its strings alias the
// tabula tree parsed into the same arena — no copies, one lifetime.

namespace ui {
namespace ir {
using namespace math;
using layout::Align;
using layout::Direction;
using layout::ImageId;
using layout::Padding;

// A pre-parsed script size: cap[:weight] per axis.
//
// The cap is a ceiling — pixels, N% of the parent, or grow for none; fit is
// sugar for weight 0. The weight is the element's share of the parent's
// leftover space, 0 meaning "fit content". Every element starts at its
// content floor, grows by weight, and stops at its cap.
//
// Zero value = fit content, uncapped. The compiler writes every node's
// final size — widget defaults are its policy, not the walk's.
struct Size {
    // The ceiling: pixels, or a 0..1 fraction of the parent when fraction
    // is set. 0 = uncapped.
    f32 cap;
    b32 fraction;
    // Share of the parent's leftover space; 0 = fit content.
    f32 weight;

    auto operator<=>(const Size&) const = default;
};

inline constexpr Size GROW = {0, false, 1}; // fill the parent: uncapped, weight 1
inline constexpr Size FIT  = {};            // fit content: the zero value

// One piece of a pre-tokenized string: either a plain literal (var is
// empty) or a $VAR reference, in which case literal keeps the source
// spelling ("$NAME") as the fallback when the binding is missing. The
// strings point into the compile arena (substrings of the tabula tree).
struct Seg {
    String literal;
    String var;
};

// A string with its $VAR references found at compile time, so per-frame
// interpolation never rescans: each Text owns its own arena-allocated seg
// array. Zero value = no text.
struct Text {
    Slice<Seg> segs;
};

// A script color, decided as early as possible. If name is non-empty, it is
// a $VAR palette name — interpolate it per frame and look it up in
// UiModule::palette; if it is empty or fails to resolve, color applies
// (literal names bake into it at compile time; for a $VAR it holds the
// widget's default). Deliberately not an enum: the variable case falls back
// on color, so the fields overlap. Zero value = no color (alpha 0), which
// every consumer treats as "draw nothing".
struct Paint {
    Text  name;
    Color color;
};

fn Paint paint_of(Color color) { return {{}, color}; }

// One fat struct covers every widget; unused fields stay at their zero
// value and the walk applies them all — there is no kind to dispatch on.
// Children are index links into the module's flat node array, with 0 (the
// reserved null node) meaning "none".
struct UiNode {
    u32 first_child;
    u32 next_sibling;
    // Element identity: hover/scroll state, and — with a template — which
    // data list this node stamps.
    Text id;
    // Click action; non-empty makes the element clickable (click events,
    // the hover skin).
    Text action;
    // Visibility, resolved per frame: the interpolated text must be empty
    // or "yes" to show the element. Conditions come from data via $VAR (row
    // bindings, then globals); a missing binding keeps its $NAME spelling
    // and hides. Zero value = shown.
    Text visible;
    // Interactivity, resolved per frame on the same channel as visible:
    // empty or "yes" = enabled. Disabled elements draw dimmed, sense
    // nothing and emit no clicks, and the state cascades to their children.
    // Zero value = enabled.
    Text enabled;
    // Hover tooltip text; empty = none.
    Text tooltip;

    // The box: where the element sits and how much room it takes.
    // floating = yes lifts the element out of its parent's flow and pins it
    // at x_pos/y_pos. Always set for top-level panels.
    b32 floating;
    // Floating elements: position as a fraction of the parent (the screen
    // for top-level panels). 0 = flush left/top, 0.5 = centered, 1 = flush
    // right/bottom.
    f32       x_pos;
    f32       y_pos;
    Size      width;
    Size      height;
    f32       min_width;  // 0 = unconstrained, all four
    f32       max_width;
    f32       min_height;
    f32       max_height;
    Direction direction;
    // Both axes come from the single align key; zero = start.
    Align   align_x;
    Align   align_y;
    Padding padding;
    f32     gap;

    // The skin.
    Paint background;
    Color hover_background; // replaces background while hovered; alpha 0 = none
    // Replaces both while the pointer holds the element down — for the
    // whole press, not just its first frame; alpha 0 = no press skin.
    Color press_background;
    f32   border_width; // 0 = no border
    Color border_color;
    f32   corner_radius;

    // Text content.
    Text  text;
    u16   text_size;
    Paint color; // the ink; role defaults are baked in, so never "unset"
    b32   wrap;  // wrap to the element width

    // Image content: the key resolved through UiData::images.
    Text  image;
    Paint tint; // alpha 0 = untinted
    f32   fade; // 0 = fully opaque

    // Interaction.
    b32 scroll_x;
    b32 scroll_y;
    // Subtree stamped once per row of the bound data list; 0 = none.
    // Reached only through this link, never through the sibling chain.
    u32 template_node;
};

// The tooltip bubble's look, baked from the style. The bubble itself is
// synthesized by the walk (it has per-frame text), so its style lives at
// module level rather than on a node.
struct Bubble {
    Color background;
    Color border;
    Color ink;
    u16   text_size;
};

// A compiled UI description plus everything wrong with it, arena-backed.
// Reload = reset the arena, compile a new one, assign over the old.
struct UiModule {
    // Flat node array. Node 0 is the null node; the top-level panels hang
    // off its first_child chain.
    vec::Vec<UiNode> nodes;
    // The style palette, for $VAR color names that only resolve per frame;
    // literal names are baked into the nodes directly.
    style::Palette palette;
    Bubble         bubble;
    // Non-fatal validation findings (unknown keys, missing '=', ...), as
    // key paths — tabula nodes carry no source positions.
    vec::Vec<String>          warnings;
    Slice<tabula::ParseError> errors;
};

// Index of the first top-level panel, 0 if there are none.
fn u32 roots(const UiModule* module) { return module->nodes.len ? module->nodes[0].first_child : 0; }

// ----------------------------------------------------------------- compiler

// Keys that declare child widgets rather than properties, allowed wherever
// widgets can nest.
inline constexpr const char* ELEMENT_KEYS[] = {"panel", "row",    "box",   "label", "heading",
                                               "section", "button", "image", "list"};

// Properties every container (panel, row, box, list) understands.
inline constexpr const char* CONTAINER_PROPS[] = {
    "visible",    "enabled",  "width",   "height",   "min_width", "max_width", "min_height",
    "max_height", "direction", "align",  "padding",  "gap",       "background", "background_image",
    "border",     "scrollable", "tooltip", "floating", "x_pos",   "y_pos",     "id"};

inline constexpr const char* LABEL_PROPS[] = {"id",         "text",       "size",      "color",
                                              "wrap",       "width",      "height",    "min_width",
                                              "max_width",  "min_height", "max_height", "visible",
                                              "enabled",    "tooltip"};

inline constexpr const char* BUTTON_PROPS[] = {"action",    "id",         "text",       "width",
                                               "height",    "min_width",  "max_width",  "min_height",
                                               "max_height", "tooltip",   "visible",    "enabled"};

inline constexpr const char* IMAGE_PROPS[] = {"source",    "id",         "width",      "height",
                                              "min_width", "max_width",  "min_height", "max_height",
                                              "tint",      "fade",       "background", "border",
                                              "tooltip",   "visible",    "enabled"};

inline constexpr const char* TEMPLATE_EXTRA[] = {"template"};

struct Compiler {
    arena::Arena* arena;
    // Built in place; compile returns it.
    UiModule module;
    // Baked into the nodes: the compiler is the only consumer.
    style::Style style;
};

using WidgetFn = u32 (*)(Compiler*, const tabula::Node*, String);

u32 elements(Compiler* c, const tabula::Node* src, String path);
u32 panel(Compiler* c, const tabula::Node* src, String path, b32 top_level);

fn b32 key_in(String key, const char* const* names, usize count) {
    for (usize i = 0; i < count; ++i) {
        if (string::equals(key, names[i])) return true;
    }
    return false;
}

fn u32 push_node(Compiler* c, UiNode node) {
    u32 index = (u32)c->module.nodes.len;
    vec::push(&c->module.nodes, node);
    return index;
}

fn void warn(Compiler* c, String message) { vec::push(&c->module.warnings, message); }

fn void warn_misplaced(Compiler* c, const tabula::Node* node, String path) {
    if (!node->key.len) {
        warn(c, string::format(c->arena, "%.*s: bare value or anonymous block — missing '='?", (int)path.len,
                               path.data));
    } else {
        warn(c, string::format(c->arena, "%.*s: unknown key '%.*s'", (int)path.len, path.data, (int)node->key.len,
                               node->key.data));
    }
}

// Warns about every child key that is in neither properties group nor (when
// elements is set) a widget key.
fn void check_keys(Compiler* c, const tabula::Node* src, String path, const char* const* props, usize prop_count,
                   b32 allow_elements, const char* const* extra = 0, usize extra_count = 0) {
    for (const tabula::Node& child : src->children) {
        b32 known = key_in(child.key, props, prop_count) || (extra && key_in(child.key, extra, extra_count)) ||
                    (allow_elements && key_in(child.key, ELEMENT_KEYS, sizeof(ELEMENT_KEYS) / sizeof(ELEMENT_KEYS[0])));
        if (!known) warn_misplaced(c, &child, path);
    }
}

fn b32 is_word_byte(char b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b == '_';
}

fn b32 contains_dollar(String s) {
    for (usize i = 0; i < s.len; ++i) {
        if (s.data[i] == '$') return true;
    }
    return false;
}

fn String trim(String s) {
    while (s.len && (s.data[0] == ' ' || s.data[0] == '\t')) {
        s.data += 1;
        s.len -= 1;
    }
    while (s.len && (s.data[s.len - 1] == ' ' || s.data[s.len - 1] == '\t')) s.len -= 1;
    return s;
}

// Tokenizes a source string into literal and $VAR segments. The segments
// are substrings of the source, which lives in the compile arena as part of
// the tabula tree — no copies. Each Text gets its own arena-allocated seg
// array, so nothing is invalidated by later tokenizations.
fn Text text(Compiler* c, String source) {
    if (!source.len) return {};
    vec::Vec<Seg> segs          = vec::make_vec<Seg>(c->arena, 2);
    usize         pos           = 0;
    usize         literal_start = 0;
    while (pos < source.len) {
        if (source.data[pos] == '$') {
            usize name_start = pos + 1;
            usize name_end   = name_start;
            while (name_end < source.len && is_word_byte(source.data[name_end])) name_end += 1;
            if (name_end > name_start) {
                if (literal_start < pos) {
                    vec::push(&segs, Seg{layout::substr(source, literal_start, pos), {}});
                }
                vec::push(&segs, Seg{layout::substr(source, pos, name_end),
                                     layout::substr(source, name_start, name_end)});
                pos           = name_end;
                literal_start = name_end;
                continue;
            }
        }
        pos += 1;
    }
    if (literal_start < source.len) {
        vec::push(&segs, Seg{layout::substr(source, literal_start, source.len), {}});
    }
    return {vec::slice(&segs)};
}

// Tokenizes a yes/no condition value (visible, enabled): yes/no or data via
// $VAR. A bare name never resolves to yes, so it would silently pin the
// condition off forever — warn instead.
fn Text condition(Compiler* c, const tabula::Node* src, const char* key, String path) {
    String value = tabula::get_text(src, key);
    if (value.len && !string::equals(value, "yes") && !string::equals(value, "no") && !contains_dollar(value)) {
        warn(c, string::format(c->arena, "%.*s: '%s = %.*s' is not yes/no or a $VAR binding", (int)path.len,
                               path.data, key, (int)value.len, value.data));
    }
    return text(c, value);
}

fn b32 yes(Compiler* c, const tabula::Node* src, const char* key, String path) {
    const tabula::Node* node = tabula::get(src, key);
    if (!node->key.len) return false;
    String value = node->value.text;
    if (string::equals(value, "yes")) return true;
    if (string::equals(value, "no")) return false;
    warn(c, string::format(c->arena, "%.*s: '%s = %.*s' is not yes/no", (int)path.len, path.data, key,
                           (int)value.len, value.data));
    return false;
}

fn Direction direction(Compiler* c, const tabula::Node* src, String path) {
    String value = tabula::get_text(src, "direction");
    if (!value.len || string::equals(value, "vertical")) return Direction::TopToBottom;
    if (string::equals(value, "horizontal")) return Direction::LeftToRight;
    warn(c, string::format(c->arena, "%.*s: 'direction = %.*s' is not vertical/horizontal", (int)path.len, path.data,
                           (int)value.len, value.data));
    return Direction::TopToBottom;
}

// Bakes one script color: absent = fallback; a literal palette name
// (including "none") bakes to its color, with unknown names warning and
// keeping the fallback; a $VAR name is kept for per-frame resolution with
// fallback as its default.
fn Paint paint(Compiler* c, const tabula::Node* src, const char* key, String path, Color fallback) {
    String name = tabula::get_text(src, key);
    if (!name.len) return paint_of(fallback);
    if (contains_dollar(name)) return {text(c, name), fallback};
    style::PaletteColor found = style::palette_color(&c->style.palette, name);
    if (found.ok) return paint_of(found.color);
    warn(c, string::format(c->arena, "%.*s: '%s = %.*s' is not a palette color", (int)path.len, path.data, key,
                           (int)name.len, name.data));
    return paint_of(fallback);
}

struct SizeResult {
    Size size;
    b32  ok;
};

// Parses one size property: cap[:weight], where the cap is a pixel number,
// N% of the parent, or grow (uncapped), and fit is sugar for weight 0.
// Absent (or bad) → not-ok, the caller's default stands.
fn SizeResult size(Compiler* c, const tabula::Node* src, const char* key, String path) {
    const tabula::Node* node = tabula::get(src, key);
    if (!node->key.len) return {};
    if (node->kind != tabula::Kind::Block && node->value.is_number) {
        // A bare number caps a default-weight grower.
        return {{max(node->value.number, 0.0f), false, 1}, true};
    }
    String full = trim(node->value.text);

    String cap_text   = full;
    f32    weight     = 0;
    b32    has_weight = false;
    for (usize i = 0; i < full.len; ++i) {
        if (full.data[i] != ':') continue;
        cap_text                     = trim(layout::substr(full, 0, i));
        string::ParseF32Result parse = string::parse_f32(trim(layout::substr(full, i + 1, full.len)));
        if (!parse.ok || parse.value < 0) {
            warn(c, string::format(c->arena, "%.*s: '%s = %.*s' has a bad weight", (int)path.len, path.data, key,
                                   (int)full.len, full.data));
            return {};
        }
        weight     = parse.value;
        has_weight = true;
        break;
    }

    Size result = {};
    if (string::equals(cap_text, "grow")) {
        result = GROW;
    } else if (string::equals(cap_text, "fit")) {
        if (has_weight && weight != 0) {
            warn(c, string::format(c->arena, "%.*s: '%s = %.*s' — fit means weight 0; drop the weight or use a cap",
                                   (int)path.len, path.data, key, (int)full.len, full.data));
        }
        return {FIT, true};
    } else if (cap_text.len && cap_text.data[cap_text.len - 1] == '%') {
        string::ParseF32Result percent = string::parse_f32(trim(layout::substr(cap_text, 0, cap_text.len - 1)));
        if (!percent.ok) {
            warn(c, string::format(c->arena, "%.*s: '%s = %.*s' has a bad percentage", (int)path.len, path.data, key,
                                   (int)full.len, full.data));
            return {};
        }
        result = {max(percent.value / 100.0f, 0.0f), true, 1};
    } else {
        // Quoted numbers skip tabula's number parsing; accept them.
        string::ParseF32Result pixels = string::parse_f32(cap_text);
        if (!pixels.ok) {
            warn(c, string::format(c->arena, "%.*s: '%s = %.*s' is not a cap[:weight] — number, N%%, grow or fit",
                                   (int)path.len, path.data, key, (int)full.len, full.data));
            return {};
        }
        result = {max(pixels.value, 0.0f), false, 1};
    }
    if (has_weight) result.weight = weight;
    return {result, true};
}

fn f32 number_or(const tabula::Node* src, const char* key, f32 fallback) {
    const tabula::Node* node = tabula::get(src, key);
    if (!node->key.len || node->kind == tabula::Kind::Block || !node->value.is_number) return fallback;
    return node->value.number;
}

// Reads the min/max constraints shared by every sized widget.
fn void constraints(Compiler* c, const tabula::Node* src, UiNode* node) {
    (void)c;
    node->min_width  = number_or(src, "min_width", node->min_width);
    node->max_width  = number_or(src, "max_width", node->max_width);
    node->min_height = number_or(src, "min_height", node->min_height);
    node->max_height = number_or(src, "max_height", node->max_height);
}

// Reads the shared container properties into node over the caller's baked
// defaults.
fn void container(Compiler* c, const tabula::Node* src, String path, UiNode* node) {
    // Floating first: it decides the size defaults below. |= so the
    // caller's top-level float can't be unset.
    if (tabula::get(src, "floating")->key.len) node->floating = node->floating || yes(c, src, "floating", path);
    node->x_pos = number_or(src, "x_pos", node->x_pos);
    node->y_pos = number_or(src, "y_pos", node->y_pos);
    // Floaters have no parent share to claim: they fit unless sized.
    if (node->floating) {
        node->width  = FIT;
        node->height = FIT;
    }
    SizeResult width = size(c, src, "width", path);
    if (width.ok) node->width = width.size;
    SizeResult height = size(c, src, "height", path);
    if (height.ok) node->height = height.size;
    constraints(c, src, node);
    if (tabula::get(src, "direction")->key.len) node->direction = direction(c, src, path);
    {
        const tabula::Node* align = tabula::get(src, "align");
        if (align->key.len) {
            String value = align->value.text;
            if (string::equals(value, "start")) {
                node->align_x = Align::Start;
                node->align_y = Align::Start;
            } else if (string::equals(value, "center")) {
                node->align_x = Align::Center;
                node->align_y = Align::Center;
            } else if (string::equals(value, "end")) {
                node->align_x = Align::End;
                node->align_y = Align::End;
            } else {
                warn(c, string::format(c->arena, "%.*s: 'align = %.*s' is not start/center/end", (int)path.len,
                                       path.data, (int)value.len, value.data));
            }
        }
    }
    if (tabula::get(src, "padding")->key.len) node->padding = layout::pad_all(number_or(src, "padding", 0));
    node->gap        = number_or(src, "gap", node->gap);
    node->background = paint(c, src, "background", path, node->background.color);
    node->image      = text(c, tabula::get_text(src, "background_image"));
    if (yes(c, src, "border", path)) {
        node->border_width = 1;
        node->border_color = c->style.palette.outline;
    }
    // Scrolling runs along the flow axis.
    if (yes(c, src, "scrollable", path)) {
        node->scroll_x = node->direction == Direction::LeftToRight;
        node->scroll_y = node->direction == Direction::TopToBottom;
    }
    node->tooltip = text(c, tabula::get_text(src, "tooltip"));
    node->id      = text(c, tabula::get_text(src, "id"));
    node->visible = condition(c, src, "visible", path);
    node->enabled = condition(c, src, "enabled", path);
}

// Runs a widget compiler on src if it is a block, else warns.
fn u32 block(Compiler* c, const tabula::Node* src, String path, const char* name, WidgetFn compile_widget) {
    if (src->kind != tabula::Kind::Block) {
        warn(c, string::format(c->arena, "%.*s: '%s' must be a { ... } block", (int)path.len, path.data, name));
        return 0;
    }
    return compile_widget(c, src, string::format(c->arena, "%.*s > %s", (int)path.len, path.data, name));
}

fn u32 nested_panel(Compiler* c, const tabula::Node* src, String path) { return panel(c, src, path, false); }

fn u32 row(Compiler* c, const tabula::Node* src, String path) {
    check_keys(c, src, path, CONTAINER_PROPS, sizeof(CONTAINER_PROPS) / sizeof(CONTAINER_PROPS[0]), true);
    // A row is a panel with different defaults: horizontal, transparent, no
    // padding. Pure compile-time sugar — the walk never knows.
    UiNode node        = {};
    node.direction     = Direction::LeftToRight;
    node.width         = GROW;
    node.height        = GROW;
    node.gap           = c->style.gap;
    node.corner_radius = c->style.corner_radius;
    container(c, src, path, &node);
    u32 index = push_node(c, node);
    c->module.nodes[index].first_child = elements(c, src, path);
    return index;
}

// A box is a pre-styled cell: it fills its slot, centers its content and
// gets the accent background. Pure compile-time sugar.
fn u32 boxed(Compiler* c, const tabula::Node* src, String path) {
    check_keys(c, src, path, CONTAINER_PROPS, sizeof(CONTAINER_PROPS) / sizeof(CONTAINER_PROPS[0]), true);
    UiNode node        = {};
    node.direction     = Direction::TopToBottom;
    node.width         = GROW;
    node.height        = GROW;
    node.align_x       = Align::Center;
    node.align_y       = Align::Center;
    node.padding       = layout::pad_all(c->style.padding);
    node.gap           = c->style.gap;
    node.background    = paint_of(c->style.palette.accent);
    node.corner_radius = c->style.corner_radius;
    container(c, src, path, &node);
    u32 index = push_node(c, node);
    c->module.nodes[index].first_child = elements(c, src, path);
    return index;
}

// label, heading and section are one widget with different text roles baked
// in from the style.
fn u32 label(Compiler* c, const tabula::Node* src, String path) {
    u16   role_size  = c->style.text_size;
    Color role_color = c->style.palette.ink;
    if (string::equals(src->key, "heading")) {
        role_size  = c->style.heading_size;
        role_color = c->style.palette.ink;
    } else if (string::equals(src->key, "section")) {
        role_size  = c->style.section_size;
        role_color = c->style.palette.muted;
    }
    UiNode node = {};
    if (src->kind == tabula::Kind::Block) {
        String block_path = string::format(c->arena, "%.*s > %.*s", (int)path.len, path.data, (int)src->key.len,
                                           src->key.data);
        check_keys(c, src, block_path, LABEL_PROPS, sizeof(LABEL_PROPS) / sizeof(LABEL_PROPS[0]), false);
        node.id        = text(c, tabula::get_text(src, "id"));
        node.text      = text(c, tabula::get_text(src, "text"));
        node.text_size = tabula::get(src, "size")->key.len ? (u16)number_or(src, "size", role_size) : role_size;
        node.color     = paint(c, src, "color", block_path, role_color);
        node.wrap      = yes(c, src, "wrap", block_path);
        SizeResult width = size(c, src, "width", block_path);
        node.width       = width.ok ? width.size : FIT;
        SizeResult height = size(c, src, "height", block_path);
        node.height       = height.ok ? height.size : FIT;
        node.visible = condition(c, src, "visible", block_path);
        node.enabled = condition(c, src, "enabled", block_path);
        node.tooltip = text(c, tabula::get_text(src, "tooltip"));
        constraints(c, src, &node);
    } else {
        node.text      = text(c, src->value.text);
        node.text_size = role_size;
        node.color     = paint_of(role_color);
    }
    return push_node(c, node);
}

fn u32 button(Compiler* c, const tabula::Node* src, String path) {
    check_keys(c, src, path, BUTTON_PROPS, sizeof(BUTTON_PROPS) / sizeof(BUTTON_PROPS[0]), false);
    Text caption = text(c, tabula::get_text(src, "text"));
    // Unsized buttons grow into the style's default caps.
    UiNode node = {};
    node.action = text(c, tabula::get_text(src, "action"));
    node.id     = text(c, tabula::get_text(src, "id"));
    SizeResult width = size(c, src, "width", path);
    node.width       = width.ok ? width.size : Size{c->style.button_width, false, 1};
    SizeResult height = size(c, src, "height", path);
    node.height       = height.ok ? height.size : Size{c->style.button_height, false, 1};
    node.align_x          = Align::Center;
    node.align_y          = Align::Center;
    node.padding          = layout::pad_symmetric(10, 4);
    node.background       = paint_of(c->style.button_background);
    node.hover_background = c->style.button_hover;
    node.press_background = c->style.button_press;
    node.border_width     = c->style.button_border_thickness;
    node.border_color     = c->style.button_border_color;
    node.corner_radius    = c->style.button_corner_radius;
    node.tooltip          = text(c, tabula::get_text(src, "tooltip"));
    node.visible          = condition(c, src, "visible", path);
    node.enabled          = condition(c, src, "enabled", path);
    constraints(c, src, &node);
    u32 index = push_node(c, node);
    if (caption.segs.len) {
        // The caption is a plain child node, centered by the button's own
        // alignment.
        UiNode caption_node    = {};
        caption_node.text      = caption;
        caption_node.text_size = c->style.text_size;
        caption_node.color     = paint_of(c->style.palette.ink);
        u32 child              = push_node(c, caption_node);
        c->module.nodes[index].first_child = child;
    }
    return index;
}

fn u32 image(Compiler* c, const tabula::Node* src, String path) {
    check_keys(c, src, path, IMAGE_PROPS, sizeof(IMAGE_PROPS) / sizeof(IMAGE_PROPS[0]), false);
    if (!tabula::get(src, "source")->key.len) {
        warn(c, string::format(c->arena, "%.*s: image without a 'source' draws nothing", (int)path.len, path.data));
    }
    b32    bordered = yes(c, src, "border", path);
    UiNode node     = {};
    node.image = text(c, tabula::get_text(src, "source"));
    node.id    = text(c, tabula::get_text(src, "id"));
    // Images fit their natural size; growing is opt-in.
    SizeResult width = size(c, src, "width", path);
    node.width       = width.ok ? width.size : FIT;
    SizeResult height = size(c, src, "height", path);
    node.height       = height.ok ? height.size : FIT;
    node.tint         = paint(c, src, "tint", path, Color{});
    node.fade         = number_or(src, "fade", 0);
    node.background   = paint(c, src, "background", path, Color{});
    node.border_width = bordered ? 1.0f : 0.0f;
    node.border_color = c->style.palette.outline;
    node.tooltip      = text(c, tabula::get_text(src, "tooltip"));
    node.visible      = condition(c, src, "visible", path);
    node.enabled      = condition(c, src, "enabled", path);
    constraints(c, src, &node);
    return push_node(c, node);
}

fn u32 list(Compiler* c, const tabula::Node* src, String path) {
    check_keys(c, src, path, CONTAINER_PROPS, sizeof(CONTAINER_PROPS) / sizeof(CONTAINER_PROPS[0]), false,
               TEMPLATE_EXTRA, 1);
    UiNode node        = {};
    node.direction     = Direction::TopToBottom;
    node.width         = GROW;
    node.height        = GROW;
    node.gap           = c->style.gap;
    node.corner_radius = c->style.corner_radius;
    // Padding and background stay zero: a list is stamping machinery, not a
    // visual box, unless the script says otherwise.
    container(c, src, path, &node);
    u32 index = push_node(c, node);

    const tabula::Node* template_src = tabula::get(src, "template");
    if (!template_src->key.len) {
        warn(c, string::format(c->arena, "%.*s: list without a template stamps nothing", (int)path.len, path.data));
    } else if (template_src->kind != tabula::Kind::Block) {
        warn(c, string::format(c->arena, "%.*s: 'template' must be a { ... } block", (int)path.len, path.data));
    } else {
        String template_path = string::format(c->arena, "%.*s > template", (int)path.len, path.data);
        check_keys(c, template_src, template_path, 0, 0, true);
        // A pure anchor for the stamped subtree: the walk splices the
        // template's elements straight into the list, so the node itself
        // never reaches layout.
        u32 template_index = push_node(c, UiNode{});
        c->module.nodes[template_index].first_child = elements(c, template_src, template_path);
        c->module.nodes[index].template_node        = template_index;
    }
    return index;
}

fn u32 panel(Compiler* c, const tabula::Node* src, String path, b32 top_level) {
    check_keys(c, src, path, CONTAINER_PROPS, sizeof(CONTAINER_PROPS) / sizeof(CONTAINER_PROPS[0]), true);
    UiNode node = {};
    // Unlike rows, panels stack vertically unless told otherwise.
    node.direction     = Direction::TopToBottom;
    node.width         = GROW;
    node.height        = GROW;
    node.padding       = layout::pad_all(c->style.padding);
    node.gap           = c->style.gap;
    node.background    = paint_of(c->style.palette.panel);
    node.corner_radius = c->style.corner_radius;
    // Top-level panels have nothing to be in flow with: always floating.
    node.floating = top_level;
    container(c, src, path, &node);
    u32 index = push_node(c, node);
    c->module.nodes[index].first_child = elements(c, src, path);
    return index;
}

// Compiles the widget children of a block into a sibling chain, returning
// the first index. Property keys are skipped silently — the caller's
// check_keys pass already vetted them.
fn u32 elements(Compiler* c, const tabula::Node* src, String path) {
    u32 first = 0;
    u32 last  = 0;
    for (const tabula::Node& child : src->children) {
        u32 index = 0;
        if (string::equals(child.key, "panel")) index = block(c, &child, path, "panel", nested_panel);
        else if (string::equals(child.key, "row")) index = block(c, &child, path, "row", row);
        else if (string::equals(child.key, "box")) index = block(c, &child, path, "box", boxed);
        else if (string::equals(child.key, "label") || string::equals(child.key, "heading") ||
                 string::equals(child.key, "section")) index = label(c, &child, path);
        else if (string::equals(child.key, "button")) index = block(c, &child, path, "button", button);
        else if (string::equals(child.key, "image")) index = block(c, &child, path, "image", image);
        else if (string::equals(child.key, "list")) index = block(c, &child, path, "list", list);
        if (index == 0) continue;
        if (first == 0) {
            first = index;
        } else {
            c->module.nodes[last].next_sibling = index;
        }
        last = index;
    }
    return first;
}

// Parse and compile a UI description against a style. Never fails: whatever
// could be recovered is compiled, with the rest reported in errors/warnings.
// Everything (module and tabula tree alike) lands in the given arena; reset
// it between reloads.
fn UiModule compile(arena::Arena* arena, String source, const style::Style* style) {
    tabula::ParseResult parsed = tabula::parse(arena, source);

    Compiler c        = {};
    c.arena           = arena;
    c.style           = *style;
    c.module.nodes    = vec::make_vec<UiNode>(arena, 32);
    c.module.warnings = vec::make_vec<String>(arena, 4);
    vec::push(&c.module.nodes, UiNode{}); // node 0: the null node

    c.module.palette = style->palette;
    c.module.bubble  = {
        .background = style->tooltip_background,
        .border     = style->palette.outline,
        .ink        = style->tooltip_ink,
        .text_size  = style->tooltip_size,
    };

    u32 last = 0;
    for (usize index = 0; index < parsed.roots.len; ++index) {
        const tabula::Node* root = &parsed.roots[index];
        String path = string::format(arena, "panel #%u", (u32)(index + 1));
        if (!string::equals(root->key, "panel") || root->kind != tabula::Kind::Block) {
            warn_misplaced(&c, root, "top level");
            continue;
        }
        u32 node = panel(&c, root, path, true);
        if (last == 0) {
            c.module.nodes[0].first_child = node;
        } else {
            c.module.nodes[last].next_sibling = node;
        }
        last = node;
    }

    c.module.errors = parsed.errors;
    return c.module;
}

} // namespace ir
} // namespace ui
