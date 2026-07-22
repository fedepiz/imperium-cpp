#pragma once
#include "../core.hpp"
#include "../arena.hpp"
#include "../file_io.hpp"
#include "../string.hpp"
#include "../vec.hpp"
#include "layout.hpp"
#include "style.hpp"
#include "ir.hpp"
#include "data.hpp"
#include "walk.hpp"

// The game-facing surface of the ui module: one system struct and two
// operations. Everything else in ui/ is machinery behind it.
//
//     ui::Ui hud = {};
//     for (String problem : ui::load(&hud, "data/ui.txt", "data/style.txt")) {
//         LOG("ui: %.*s", (int)problem.len, problem.data);
//     }
//     // per frame:
//     ui::data::Data data = ui::data::make(&frame_arena);
//     ui::data::bind_global(&data, "DATE", "Year 700");
//     ui::Result frame = ui::run(&hud, &frame_arena, input, &data, measure);
//     for (String action : frame.actions) { ... }
//     // render frame.commands, gate world clicks on frame.pointer_over_ui
//
// The facade stays renderer-free: the caller supplies the measure callable
// and draws the returned commands itself.

namespace ui {

// The caller-facing engine types, hoisted so the game spells one namespace.
// The layers keep their definitions; tests still use ui::layout directly.
using layout::DrawCommand;
using layout::DrawKind;
using layout::Input;
using layout::TextMetrics;

// The UI system: the compiled module and the layout engine, plus the arena
// backing the module. ZII — load() wires it; after the first load the
// struct must stay put (the engine's containers point back into it).
struct Ui {
    arena::Arena   arena; // module storage; reset by every load
    ir::UiModule   module;
    layout::Engine engine;
};

// One frame's product, fully self-contained in the arena run() was given:
// commands (text included) and actions are cloned there, so the Result
// outlives the engine's internals and dies only with that arena.
struct Result {
    Slice<DrawCommand> commands;
    Slice<String>      actions; // interpolated action ids of everything clicked, in click order
    b32                pointer_over_ui;
};

constexpr usize UI_MODULE_ARENA_SIZE = 16 * 1024 * 1024;

// (Re)loads the UI: resets the module arena, reads and compiles the script
// and its style, and returns every problem found — file errors, parse
// errors, style and compile warnings — as formatted strings in the module
// arena (valid until the next load). An empty slice is a clean load; on
// problems whatever could be recovered is live. Hot reload is simply
// calling this again.
fn Slice<String> load(Ui* ui, String ui_path, String style_path) {
    if (!ui->arena.base) arena::reserve(&ui->arena, UI_MODULE_ARENA_SIZE);
    arena::Arena* arena = &ui->arena;
    arena::reset(arena);
    vec::Vec<String> problems = vec::make_vec<String>(arena, 4);

    style::Style              style        = style::default_style();
    file_io::ReadFile<String> style_source = file_io::read_file_to_string(arena, style_path);
    if (style_source.messages.len) {
        String message = (*style_source.messages.begin()).message;
        vec::push(&problems, string::format(arena, "%.*s: %.*s — using the built-in style", (int)style_path.len,
                                            style_path.data, (int)message.len, message.data));
    } else {
        style::StyleModule parsed = style::parse(arena, style_source.data);
        for (String warning : parsed.warnings) {
            vec::push(&problems, string::format(arena, "%.*s: %.*s", (int)style_path.len, style_path.data,
                                                (int)warning.len, warning.data));
        }
        style = parsed.style;
    }

    file_io::ReadFile<String> source = file_io::read_file_to_string(arena, ui_path);
    if (source.messages.len) {
        String message = (*source.messages.begin()).message;
        vec::push(&problems, string::format(arena, "%.*s: %.*s — empty ui", (int)ui_path.len, ui_path.data,
                                            (int)message.len, message.data));
    }
    ui->module = ir::compile(arena, source.data, &style);
    for (const tabula::ParseError& error : ui->module.errors) {
        vec::push(&problems, string::format(arena, "%.*s: %.*s", (int)ui_path.len, ui_path.data,
                                            (int)error.message.len, error.message.data));
    }
    for (String warning : vec::slice(ui->module.warnings)) {
        vec::push(&problems, string::format(arena, "%.*s: %.*s", (int)ui_path.len, ui_path.data, (int)warning.len,
                                            warning.data));
    }
    return vec::slice(problems);
}

// One frame: lays the module out against the data and input, and returns
// the draw commands and clicked actions, all cloned into the given arena
// (typically the caller's frame arena — interpolation scratch lands there
// too). Duplicate element ids are a declaration bug and are LOGged.
template <layout::MeasureText M>
fn Result run(Ui* ui, arena::Arena* arena, Input input, const data::Data* data, M measure) {
    Slice<String>   actions = {};
    layout::Output* output  = layout::layout(&ui->engine, input, measure, [&](layout::Ui* u) {
        actions = walk::run(&ui->module, data, arena, u);
    });
    if (output->duplicate_ids.len) LOG("ui: %d duplicate element ids", (int)output->duplicate_ids.len);

    // Clone everything the caller keeps: the Result must not reach back
    // into the engine (whose buffers recycle at the next layout) or the
    // module (which dies at the next load).
    Slice<DrawCommand> commands = arena::clone_slice(arena, vec::slice(output->commands));
    for (DrawCommand& command : commands) {
        if (command.text.len) command.text = arena::clone_string(arena, command.text);
    }
    Slice<String> cloned_actions = arena::clone_slice(arena, actions);
    for (String& action : cloned_actions) action = arena::clone_string(arena, action);

    return {commands, cloned_actions, output->is_pointer_over_ui};
}

} // namespace ui
