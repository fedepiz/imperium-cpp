#pragma once
#include "../core.hpp"
#include "../arena.hpp"
#include "../math.hpp"
#include "../string.hpp"
#include "../vec.hpp"
#include "layout.hpp"
#include "style.hpp"
#include "data.hpp"
#include "ir.hpp"

// Per-frame interpreter for a compiled UiModule: walks the IR, emits layout
// elements, and collects the action ids of everything clicked.
//
// The walk is kind-blind — the compiler baked every decision (defaults,
// style values, what may float) into the nodes, so element() applies every
// field of every node the same way. Widget identity does not exist here.
//
// Per-frame scratch (interpolated strings) lands in the caller's frame
// arena, and so do the returned events — consume or copy them before the
// arena resets.

namespace ui {
namespace walk {
using namespace math;

// Everything the walk threads along, one fat struct.
struct Ctx {
    arena::Arena*       frame;
    const ir::UiModule* module;
    const data::Data*   data;
    vec::Vec<String>    events;
    // Counter for elements with no script id that still need one (an
    // action, scrolling, a tooltip); declaration order is deterministic, so
    // the synthesized ids are stable across frames.
    u32 auto_id;
    // Bindings of the template row being stamped; zero outside lists.
    data::Row row;
    // True inside a disabled element's subtree: disabling cascades, so a
    // disabled button's caption dims with it.
    b32 disabled;
};

void walk(Ctx* ctx, layout::Ui* u, u32 index);
void stamp(Ctx* ctx, layout::Ui* u, const ir::UiNode* node);

struct Lookup {
    String value;
    b32    ok;
};

// Looks a $VAR up in the current row's bindings, then in the globals (the
// root scope every element sees); a row binding shadows a global of the
// same key.
fn Lookup lookup(const Ctx* ctx, String var) {
    for (const data::Binding& binding : data::bindings(ctx->data, ctx->row)) {
        if (binding.key == var) return {binding.value, true};
    }
    for (const data::Binding& binding : ctx->data->globals) {
        if (binding.key == var) return {binding.value, true};
    }
    return {};
}

// Interpolates a pre-tokenized string against the current row and the
// globals. Missing bindings keep their $NAME spelling so mistakes show up
// on screen.
fn String resolve(const Ctx* ctx, ir::Text text) {
    if (text.segs.len == 0) return {};
    // A plain literal needs no interpolation: the module outlives the
    // frame, so its string is returned as-is, no copy.
    if (text.segs.len == 1 && text.segs[0].var.len == 0) return text.segs[0].literal;
    vec::Vec<char> out = vec::make_vec<char>(ctx->frame, 32);
    for (const ir::Seg& seg : text.segs) {
        if (seg.var.len) {
            Lookup found = lookup(ctx, seg.var);
            if (found.ok) {
                vec::push_all(&out, found.value);
                continue;
            }
        }
        vec::push_all(&out, seg.literal);
    }
    return vec::slice(out);
}

// Resolves a paint. Almost always baked: only a $VAR name costs anything,
// interpolated and looked up in the module's palette with the baked color
// as the fallback.
fn Color resolve_paint(const Ctx* ctx, ir::Paint paint) {
    if (paint.name.segs.len == 0) return paint.color;
    String              name  = resolve(ctx, paint.name);
    style::PaletteColor found = style::palette_color(&ctx->module->palette, name);
    return found.ok ? found.color : paint.color;
}

// One axis mapped onto the engine's vocabulary: the growth mode, plus the
// pixel and fractional caps to constrain it with.
struct AxisResult {
    layout::Size mode;
    f32          cap;
    f32          fraction;
};

fn AxisResult axis(ir::Size size) {
    layout::Size mode = size.weight == 0 ? layout::Size{} : layout::grow(size.weight);
    if (size.fraction) return {mode, 0, size.cap};
    return {mode, size.cap, 0};
}

// Two ceilings, 0 = none each; the smaller real one wins (the engine folds
// the fractional cap in the same way).
fn f32 tighter(f32 a, f32 b) { return (a > 0 && b > 0) ? min(a, b) : max(a, b); }

// Applies both size axes onto the conf. Caps combine with the explicit
// min_*/max_* keys.
fn void apply_size(layout::ElementConf* conf, const ir::UiNode* node) {
    AxisResult width     = axis(node->width);
    conf->width          = width.mode;
    conf->max_width      = tighter(width.cap, node->max_width);
    conf->max_fraction.x = width.fraction;
    conf->min_width      = node->min_width;

    AxisResult height    = axis(node->height);
    conf->height         = height.mode;
    conf->max_height     = tighter(height.cap, node->max_height);
    conf->max_fraction.y = height.fraction;
    conf->min_height     = node->min_height;
}

struct ImageLookup {
    data::ImageData data;
    b32           ok;
};

fn ImageLookup find_image(const Ctx* ctx, ir::Text key_text) {
    String key = resolve(ctx, key_text);
    for (const data::ImageData& image : ctx->data->images) {
        if (image.key == key) return {image, true};
    }
    return {};
}

// The stable identity an interactive element senses under: the script's id,
// else its action (so buttons keep their identity across frames), else one
// synthesized from declaration order.
fn layout::ElementId element_id(Ctx* ctx, const ir::UiNode* node, String action) {
    String scripted = resolve(ctx, node->id);
    if (scripted.len) return layout::ElementId(scripted);
    if (action.len) return layout::ElementId(action);
    ctx->auto_id += 1;
    return layout::ElementId(String("__ui"), ctx->auto_id);
}

// Declares the floating tooltip bubble inside a hovered element.
fn void bubble(Ctx* ctx, layout::Ui* u, ir::Text tooltip) {
    ir::Bubble style = ctx->module->bubble;
    String     text  = resolve(ctx, tooltip);
    layout::add_with(u,
                     {
                         // Bottom-center of the bubble pinned above the
                         // element's top-center.
                         .padding       = layout::pad_symmetric(10, 6),
                         .background    = style.background,
                         .border        = {.width = 1, .color = style.border, .radius = 6},
                         .floating      = true,
                         .z_index       = 10,
                         .anchor_parent = {0.5f, 0},
                         .anchor_self   = {0.5f, 1},
                         .float_offset  = {0, -8},
                     },
                     [&] { layout::add(u, {.text = {.text = text, .size = style.text_size, .color = style.ink}}); });
}

// Evaluates a node's visible condition: unset = shown, otherwise the
// interpolated text must be "yes". Conditions come from data (visible =
// "$OPEN" against row bindings or globals); a missing binding keeps its
// $NAME spelling, so conditional elements stay hidden until the fill code
// opts them in.
fn b32 is_visible(const Ctx* ctx, const ir::UiNode* node) {
    String value = resolve(ctx, node->visible);
    return value.len == 0 || value == "yes";
}

// Same channel as visible: unset = enabled, otherwise "yes".
fn b32 is_enabled(const Ctx* ctx, const ir::UiNode* node) {
    String value = resolve(ctx, node->enabled);
    return value.len == 0 || value == "yes";
}

// The disabled skin: everything keeps its color, at a fraction of its alpha.
fn Color dimmed(Color color, b32 disabled) {
    if (disabled) color.a = (u8)((f32)color.a * 0.4f);
    return color;
}

// Lowers one node onto the engine, kind-blind: every field of every node
// applies. What the fields mean was the compiler's decision; here they are
// only carried out.
fn void element(Ctx* ctx, layout::Ui* u, const ir::UiNode* node) {
    // Anything can be interactive: an action, a hover skin, scrolling or a
    // tooltip needs a stable id and hover sensing (sense-then-declare:
    // styling reads last frame's bounds). Disabled elements sense nothing.
    b32    disabled    = ctx->disabled || !is_enabled(ctx, node);
    String action      = resolve(ctx, node->action);
    b32    interactive = !disabled && (action.len || node->hover_background.a > 0 || node->press_background.a > 0 ||
                                    node->scroll_x || node->scroll_y || node->tooltip.segs.len);
    layout::ElementId id    = {};
    layout::Sense     sense = {};
    if (interactive) {
        id    = element_id(ctx, node, action);
        sense = layout::sense(u, id);
    }

    String              text = resolve(ctx, node->text);
    layout::ElementConf conf = {};
    conf.id                  = id; // zero when not interactive: a structural auto-id
    if (text.len) {
        conf.text = {text, node->text_size, dimmed(resolve_paint(ctx, node->color), disabled), node->wrap};
    }
    apply_size(&conf, node);
    conf.direction     = node->direction;
    conf.align_x       = node->align_x;
    conf.align_y       = node->align_y;
    conf.padding       = node->padding;
    conf.gap           = node->gap;
    conf.border.radius = node->corner_radius;
    if (sense.held && node->press_background.a > 0) {
        conf.background = node->press_background;
    } else if (sense.hovered && node->hover_background.a > 0) {
        conf.background = node->hover_background;
    } else {
        conf.background = dimmed(resolve_paint(ctx, node->background), disabled);
    }
    if (node->image.segs.len) {
        ImageLookup image = find_image(ctx, node->image);
        if (image.ok) {
            conf.image        = image.data.image;
            conf.image_source = {image.data.width, image.data.height};
            conf.image_tint   = resolve_paint(ctx, node->tint);
            // Disabled images dim through the fade channel.
            conf.image_fade = disabled ? max(node->fade, 0.6f) : node->fade;
        }
    }
    if (node->border_width > 0) {
        conf.border.width = node->border_width;
        conf.border.color = dimmed(node->border_color, disabled);
    }
    conf.scroll_x = node->scroll_x;
    conf.scroll_y = node->scroll_y;
    if (node->floating) {
        // The same fraction on parent and self gives the script's
        // positioning semantics: 0 = flush, 0.5 = centered, 1 = flush
        // right/bottom. The parent is the screen for top-level panels.
        conf.floating      = true;
        conf.anchor_parent = {node->x_pos, node->y_pos};
        conf.anchor_self   = {node->x_pos, node->y_pos};
    }

    // Stack discipline: the subtree inherits this element's disabled state,
    // siblings get the caller's back.
    b32 outer_disabled = ctx->disabled;
    ctx->disabled      = disabled;
    layout::add_with(u, conf, [&] {
        walk(ctx, u, node->first_child);
        if (node->template_node != 0) stamp(ctx, u, node);
        if (sense.hovered && node->tooltip.segs.len) bubble(ctx, u, node->tooltip);
    });
    ctx->disabled = outer_disabled;
    if (sense.clicked && action.len) vec::push(&ctx->events, action);
}

fn void walk(Ctx* ctx, layout::Ui* u, u32 index) {
    while (index != 0) {
        const ir::UiNode* node = &ctx->module->nodes[index];
        if (is_visible(ctx, node)) element(ctx, u, node);
        index = node->next_sibling;
    }
}

// Stamps the node's template once per row of its bound data list (matched
// by the node's interpolated id). Pure splicing: each instance's elements
// land directly in the list container, as if the template's body had been
// written out once per row — no wrapper element, so the list lays out
// stamped children exactly like literal ones. Templates that want a per-row
// unit (a background, a hover surface) declare their own container, and
// per-row identity that must survive reordering comes from interpolated ids
// on the elements themselves.
fn void stamp(Ctx* ctx, layout::Ui* u, const ir::UiNode* node) {
    String         key          = resolve(ctx, node->id);
    Slice<data::Row> stamped_rows = {};
    for (const data::ListData& list : ctx->data->lists) {
        if (list.id == key) {
            stamped_rows = data::rows(ctx->data, list);
            break;
        }
    }
    const ir::UiNode* template_node = &ctx->module->nodes[node->template_node];
    // Stack discipline: a nested list must not clobber the row its siblings
    // in the enclosing template still resolve against.
    data::Row outer_row = ctx->row;
    for (const data::Row& row : stamped_rows) {
        ctx->row = row;
        walk(ctx, u, template_node->first_child);
    }
    ctx->row = outer_row;
}

// Walks the module's panels, declaring them into the current layout pass.
// Returns the interpolated action ids of every button clicked this frame,
// in click order — frame-arena backed, valid until the arena resets.
fn Slice<String> run(const ir::UiModule* module, const data::Data* data, arena::Arena* frame, layout::Ui* u) {
    Ctx ctx          = {};
    ctx.frame        = frame;
    ctx.module       = module;
    ctx.data         = data;
    ctx.events.arena = frame;
    walk(&ctx, u, ir::roots(module));
    return vec::slice(ctx.events);
}

} // namespace walk
} // namespace ui
