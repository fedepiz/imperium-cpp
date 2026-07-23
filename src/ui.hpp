#pragma once
#include "arena.hpp"
#include "vec.hpp"
#include "clay.h"
#include "core.hpp"
#include "math.hpp"
#include "sheet.hpp"
#include "tabula.hpp"

namespace ui {
using namespace math;

enum class SizeKind : u8 {
    Fit,    // size to contents — clay's default; amount is a pixel floor
    Pixels, // amount is an exact size in pixels
    Parent, // amount is a fraction of the parent's size, 0..1
};

struct SizeValue {
    SizeKind kind;
    f32      amount;
};

struct Size {
    SizeValue min;
    SizeValue max;
};

enum class Axis {
    Horizontal,
    Vertical,
    Count,
};

struct Text {
    String value;
    u16    font_size;
};

struct Node {
    String       list;
    String       id;
    Text         text;
    Size         width;
    Size         height;
    f32          child_gap;
    V2           pad;
    Axis         layout_direction;
    Slice<Node*> children;
};

struct Input {
    V2  mouse_position;
    b32 mouse_down;
};

struct Output {
    Clay_RenderCommandArray commands;
    b32                     is_pointer_over_area;
};

namespace impl {
struct Ctx {
    arena::Arena* arena;
    sheet::Sheet  sheet;
};

Clay_String to_clay(String str) {
    return {
        .isStaticallyAllocated = false,
        .chars                 = str.data,
        .length                = (int)str.len,
    };
}

fn bool is_ident(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Replaces each $NAME with the row's binding for NAME: "aaa $V1 bb $V2" with
// {V1: "c", V2: "d"} becomes "aaa c bb d". Unknown names substitute as empty;
// a '$' not followed by an identifier stays literal. Text without '$' is
// returned as-is, unallocated.
fn String interpolate(Ctx* ctx, sheet::Row row, String text) {
    b32 has_dollar = false;
    for (usize idx = 0; idx < text.len; ++idx) {
        if (text[idx] == '$') {
            has_dollar = true;
            break;
        }
    }
    if (!has_dollar) { return text; }

    vec::Vec<char> out = vec::make<char>(ctx->arena, text.len);

    usize pos = 0;
    while (pos < text.len) {
        if (text[pos] != '$') {
            vec::push(&out, text[pos]);
            pos += 1;
            continue;
        }

        usize start = pos + 1;
        usize end   = start;
        while (end < text.len && is_ident(text[end])) {
            end += 1;
        }
        if (end == start) { // lone '$' — keep it
            vec::push(&out, '$');
            pos += 1;
            continue;
        }

        String name = {end - start, text.data + start};
        vec::push_all(&out, sheet::get_str(row, name));
        pos = end;
    }
    return vec::slice(out);
}

} // namespace impl

fn void show_node(impl::Ctx* ctx, const Node* node) {
    // Get the data rows
    sheet::Row        global = sheet::get_global(ctx->sheet);
    Slice<sheet::Row> rows   = {1, &global};
    if (node->list != "") { rows = sheet::get_list(ctx->sheet, node->list); }

    for (auto row : rows) {

        Clay_ElementId id = {};
        {
            String text = impl::interpolate(ctx, row, node->id);
            if (text.len) { id = Clay_GetElementId(impl::to_clay(text)); }
        }

        Clay_LayoutConfig layout = {};

        Clay_LayoutDirection direction = {};
        switch (node->layout_direction) {
            case Axis::Vertical: direction = CLAY_TOP_TO_BOTTOM; break;
            case Axis::Horizontal: direction = CLAY_LEFT_TO_RIGHT; break;
            default:;
        }
        layout.layoutDirection = direction;

        layout.childGap = (u16)node->child_gap;

        layout.padding.left   = (u16)node->pad.x;
        layout.padding.right  = (u16)node->pad.x;
        layout.padding.top    = (u16)node->pad.y;
        layout.padding.bottom = (u16)node->pad.y;

        Clay_ElementDeclaration elem = {.id = id, .layout = layout};

        // Recurse on children
        CLAY(elem) {
            if (node->text.value.len > 0) {
                Clay_TextElementConfig cfg = {};

                cfg.textColor     = {0, 0, 0, 255};
                cfg.fontSize      = node->text.font_size;
                cfg.textAlignment = CLAY_TEXT_ALIGN_CENTER;

                String value = impl::interpolate(ctx, row, node->text.value);

                CLAY_TEXT(impl::to_clay(value), CLAY_TEXT_CONFIG({cfg}));
            }

            for (auto child : node->children) {
                show_node(ctx, child);
            }
        }
    }
}

fn Output show(Input input, Slice<Node> nodes, sheet::Sheet data, arena::Arena* arena) {
    Output out = {};
    Clay_SetPointerState({input.mouse_position.x, input.mouse_position.y}, input.mouse_down);
    Clay_BeginLayout();

    impl::Ctx ctx;
    ctx.arena = arena;
    ctx.sheet = data;

    for (const auto& node : nodes) {
        show_node(&ctx, &node);
    }
    out.commands = Clay_EndLayout();
    return out;
}

namespace parser {

Node* parse_node(arena::Arena* arena, tabula::Node* def) {
    Node node = {};

    {
        node.text.value     = arena::clone_string(arena, tabula::get_text(def, "text"));
        node.text.font_size = tabula::get_number(def, "font_size");
    }

    {
        auto children = vec::make<Node*>(arena, def->children.len);
        for (auto child_def : def->children) {
            auto child = parse_node(arena, &child_def);
            vec::push(&children, child);
        }
        node.children = vec::slice(children);
    }

    auto out = arena::allocate<Node>(arena, 1);
    *out     = node;
    return out;
}
} // namespace parser

Slice<Node*> parse(arena::Arena* arena, tabula::Node root) {
    auto roots = vec::make<Node*>(arena, root.children.len);
    for (auto root : root.children) {
        auto node = parser::parse_node(arena, &root);
        vec::push(&roots, node);
    }
    return vec::slice(roots);
};

}; // namespace ui
