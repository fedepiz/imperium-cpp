#pragma once
#include "../core.hpp"
#include "../arena.hpp"
#include "../math.hpp"
#include "../string.hpp"
#include "../vec.hpp"
#include "../tabula.hpp"

// The UI style, parsed from its own tabula source, once per (re)load.
//
// Style is an input to ui::ir::compile, not to the per-frame walk: role font
// sizes, paddings and palette colors are baked into the compiled nodes, and
// a style edit is a recompile like any script edit. parse mirrors compile:
// it never fails — every recognized key overrides one default_style() field,
// and anything wrong is reported as a warning while the default stands.

namespace ui {
namespace style {
using namespace math;

// Style-file colors are { r g b a } channels in 0-1; ours are u8. Rounded
// at the boundary, here.
fn constexpr Color rgba(f32 r, f32 g, f32 b, f32 a) {
    return {(u8)(r * 255.0f + 0.5f), (u8)(g * 255.0f + 0.5f), (u8)(b * 255.0f + 0.5f), (u8)(a * 255.0f + 0.5f)};
}

// The named colors scripts refer to (background = accent). A fixed,
// compile-time set — one field per name, no lookup tables. It rides in the
// compiled module so $VAR color names can still resolve per frame.
struct Palette {
    Color panel;
    Color dark;
    Color outline;
    Color ink;
    Color muted;
    // The one highlight color; everything the script wants to pop (boxes,
    // badges, tints) uses this.
    Color accent;
};

// Visuals the script format doesn't specify, plus the palette the script
// refers to by name. The zero value renders invisibly but harmlessly;
// default_style() gives the built-in look.
struct Style {
    Palette palette;
    Color   button_background;
    Color   button_hover;
    Color   button_press;
    f32     button_border_thickness; // 0 = borderless buttons
    Color   button_border_color;
    f32     button_corner_radius;
    // Default size cap for buttons the script doesn't size: they grow into
    // it. 0 = uncapped. (Labels need no counterpart — they fit their text.)
    f32   button_width;
    f32   button_height;
    Color tooltip_background;
    // Tooltip text color, separate from palette.ink — the bubble keeps its
    // own ground, so its text can't follow the panels' ink.
    Color tooltip_ink;
    u16   heading_size;
    u16   section_size;
    u16   text_size;
    u16   tooltip_size;
    f32   padding;
    f32   gap;
    f32   corner_radius;
};

fn Style default_style() {
    Style style = {};
    style.palette = {
        .panel   = rgba(0.055f, 0.067f, 0.10f, 0.9f),
        .dark    = rgba(0.03f, 0.04f, 0.07f, 1.0f),
        .outline = rgba(0.25f, 0.29f, 0.40f, 1.0f),
        .ink     = rgba(0.94f, 0.95f, 1.0f, 1.0f),
        .muted   = rgba(0.57f, 0.62f, 0.74f, 1.0f),
        .accent  = rgba(0.43f, 0.32f, 0.92f, 1.0f),
    };
    style.button_background       = rgba(0.08f, 0.68f, 0.72f, 1.0f);
    style.button_hover            = rgba(0.16f, 0.86f, 0.90f, 1.0f);
    style.button_press            = rgba(0.38f, 0.95f, 0.98f, 1.0f);
    style.button_border_thickness = 0;
    style.button_border_color     = rgba(0, 0, 0, 1.0f);
    style.button_corner_radius    = 10;
    style.button_width            = 0;
    style.button_height           = 0;
    style.tooltip_background      = rgba(0.02f, 0.03f, 0.05f, 0.95f);
    style.tooltip_ink             = rgba(0.94f, 0.95f, 1.0f, 1.0f);
    style.heading_size            = 28;
    style.section_size            = 13;
    style.text_size               = 16;
    style.tooltip_size            = 14;
    style.padding                 = 12;
    style.gap                     = 8;
    style.corner_radius           = 10;
    return style;
}

struct PaletteColor {
    Color color;
    b32   ok;
};

// Looks up a script color name. The palette is semantic on purpose —
// scripts say what a thing is, not which hue it has. "none" is "no color"
// (transparent); unknown names come back not-ok so callers can warn or
// fall back.
fn PaletteColor palette_color(const Palette* palette, String name) {
    if (name == "panel") return {palette->panel, true};
    if (name == "dark") return {palette->dark, true};
    if (name == "outline") return {palette->outline, true};
    if (name == "ink") return {palette->ink, true};
    if (name == "muted") return {palette->muted, true};
    if (name == "accent") return {palette->accent, true};
    if (name == "none") return {Color{}, true};
    return {};
}

// A parsed style plus everything wrong with it. Warnings (including the
// source's parse errors — style problems are never fatal) live in the arena
// given to parse.
struct StyleModule {
    Style         style;
    Slice<String> warnings;
};

struct ColorResult {
    Color color;
    b32   ok;
};

// Parses a { r g b } or { r g b a } array of numbers, channels in 0-1,
// alpha defaulting to 1.
fn ColorResult parse_color(const tabula::Node* node) {
    if (node->kind != tabula::Kind::Block || node->children.len < 3 || node->children.len > 4) return {};
    f32 channels[4] = {0, 0, 0, 1};
    for (usize i = 0; i < node->children.len; ++i) {
        const tabula::Node* child = &node->children[i];
        if (child->kind == tabula::Kind::Block || !child->value.is_number || child->key.len) return {};
        channels[i] = child->value.number;
    }
    return {rgba(channels[0], channels[1], channels[2], channels[3]), true};
}

// Parse a style source: flat key = value pairs, one per Style field. Colors
// are { r g b a } arrays in 0-1, alpha optional. The tabula tree parses into
// the same arena; reset it between reloads.
fn StyleModule parse(arena::Arena* arena, String source) {
    tabula::ParseResult parsed = tabula::parse(arena, source);

    vec::Vec<String> warnings = vec::make_vec<String>(arena, 4);
    for (const tabula::ParseError& error : parsed.errors) {
        vec::push(&warnings, error.message);
    }

    Style style = default_style();
    for (const tabula::Node& node : parsed.roots) {
        Color* slot = 0;
        if (node.key == "panel_background") slot = &style.palette.panel;
        else if (node.key == "dark") slot = &style.palette.dark;
        else if (node.key == "outline") slot = &style.palette.outline;
        else if (node.key == "ink") slot = &style.palette.ink;
        else if (node.key == "muted") slot = &style.palette.muted;
        else if (node.key == "accent") slot = &style.palette.accent;
        else if (node.key == "button_background") slot = &style.button_background;
        else if (node.key == "button_hover") slot = &style.button_hover;
        else if (node.key == "button_press") slot = &style.button_press;
        else if (node.key == "button_border_color") slot = &style.button_border_color;
        else if (node.key == "tooltip_background") slot = &style.tooltip_background;
        else if (node.key == "tooltip_ink") slot = &style.tooltip_ink;

        if (slot) {
            ColorResult color = parse_color(&node);
            if (color.ok) {
                *slot = color.color;
            } else {
                vec::push(&warnings, string::format(arena, "'%.*s' is not a { r g b a } color", (int)node.key.len,
                                                    node.key.data));
            }
            continue;
        }

        f32* number_slot = 0;
        u16* size_slot   = 0;
        if (node.key == "heading_size") size_slot = &style.heading_size;
        else if (node.key == "section_size") size_slot = &style.section_size;
        else if (node.key == "text_size") size_slot = &style.text_size;
        else if (node.key == "tooltip_size") size_slot = &style.tooltip_size;
        else if (node.key == "button_border_thickness") number_slot = &style.button_border_thickness;
        else if (node.key == "button_corner_radius") number_slot = &style.button_corner_radius;
        else if (node.key == "button_width") number_slot = &style.button_width;
        else if (node.key == "button_height") number_slot = &style.button_height;
        else if (node.key == "padding") number_slot = &style.padding;
        else if (node.key == "gap") number_slot = &style.gap;
        else if (node.key == "corner_radius") number_slot = &style.corner_radius;

        if (number_slot || size_slot) {
            if (node.kind != tabula::Kind::Block && node.value.is_number) {
                if (number_slot) *number_slot = node.value.number;
                if (size_slot) *size_slot = (u16)node.value.number;
            } else {
                vec::push(&warnings,
                          string::format(arena, "'%.*s' is not a number", (int)node.key.len, node.key.data));
            }
            continue;
        }

        vec::push(&warnings, string::format(arena, "unknown key '%.*s'", (int)node.key.len, node.key.data));
    }

    return {style, vec::slice(warnings)};
}

} // namespace style
} // namespace ui
