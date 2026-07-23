#include "core.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "game.hpp"
#include "math.hpp"
#include "string.hpp"
#include "tabula.hpp"
#include "ray.hpp"
#include "ui/ui.hpp"
#include "vec.hpp"
#include <math.h>

constexpr i32 MAX_SPEED = 5;

struct TimeCommand {
    b32 toggle_pause;
    i32 set_speed; // absolute level; 0 = no change (valid speeds start at 1)
};

// One frame event from any input source — the frame's key poll or a parsed
// UI action. Fat ZII struct: zero fields are no-ops, so each source fills
// only what it means and apply reads them all.
struct Command {
    b32         reload_ui;
    TimeCommand time;
    // Pan intent as a normalized direction; speed, smoothing and dt stay
    // inside camera_update.
    math::V2 camera_move;
    // Snap the camera back onto the player.
    b32 follow_player;
    // Sim-bound command, queued into this frame's TickCommands.
    game::Command game;
    // Set only when the source was garbled or unknown; apply just prints it.
    String log_msg;
};

// Run configuration, read from a tabula file. Every field has a built-in
// default; the file overrides what it names. A missing or unreadable file —
// or a garbled field — is not an error: the run proceeds on defaults, with a
// LOG for visibility.
struct Config {
    i32 screen_width;
    i32 screen_height;
    b32 vsync;
};

fn Config default_config() {
    Config result        = {};
    result.screen_width  = 1280;
    result.screen_height = 720;
    result.vsync         = true;
    return result;
}

fn Config load_config(arena::Arena* arena, String path) {
    Config result = default_config();

    file_io::ReadFile<String> source = file_io::read_file_to_string(arena, path);
    if (source.messages.len) {
        String message = (*source.messages.begin()).message;
        LOG("config: %.*s — using defaults", (int)message.len, message.data);
        return result;
    }

    // Parse errors are logged, not fatal: the parser recovers, so intact
    // fields still apply.
    tabula::ParseResult parsed = tabula::parse(arena, source.data);
    for (usize i = 0; i < parsed.errors.len; ++i) {
        String message = parsed.errors[i].message;
        LOG("config: %.*s", (int)message.len, message.data);
    }

    // Config keys are the file's top level; wrap the roots in a synthetic
    // block so tabula's lookups apply.
    tabula::Node root = {};
    root.kind         = tabula::Kind::Block;
    root.children     = parsed.roots;

    // Dimensions must be positive; zero, negative, or garbled keep defaults.
    i32 width  = tabula::read_i32(&root, "screen_width", 0);
    i32 height = tabula::read_i32(&root, "screen_height", 0);
    if (width > 0) result.screen_width = width;
    if (height > 0) result.screen_height = height;
    result.vsync = tabula::read_b32(&root, "vsync", result.vsync);
    return result;
}

// Smooth-scrolling camera: the ray::Camera is the view transform, the wrapper
// adds the velocity that eases it around. ZII: the zero camera is the
// identity view, at rest.
struct MovingCamera {
    ray::Camera inner;
    math::V2    velocity;
};

// The keyboard's contribution to the frame: one Command merging everything
// the keys say (fat struct — the unrelated fields coexist).
fn void key_input(vec::Vec<Command>* out) {
    Command command = {};
    if (ray::key_pressed(ray::Key::R)) command.reload_ui = true;
    if (ray::key_pressed(ray::Key::Space)) command.time.toggle_pause = true;
    if (ray::key_pressed(ray::Key::F)) command.follow_player = true;

    math::V2 dir = {};
    if (ray::key_down(ray::Key::Left) || ray::key_down(ray::Key::A)) dir.x -= 1;
    if (ray::key_down(ray::Key::Right) || ray::key_down(ray::Key::D)) dir.x += 1;
    if (ray::key_down(ray::Key::Up) || ray::key_down(ray::Key::W)) dir.y -= 1;
    if (ray::key_down(ray::Key::Down) || ray::key_down(ray::Key::S)) dir.y += 1;
    f32 dir_len = sqrtf(dir.x * dir.x + dir.y * dir.y);
    if (dir_len > 0) { // normalize so diagonals aren't faster
        dir.x /= dir_len;
        dir.y /= dir_len;
    }
    command.camera_move = dir;

    for (auto speed = 1; speed <= MAX_SPEED; ++speed) {
        ray::Key key = (ray::Key)((usize)ray::Key::Zero + speed);
        if (ray::key_pressed(key)) { command.time.set_speed = speed; }
    }

    vec::push(out, command);
}

// dir is the frame's pan intent; smoothing turns it into velocity. Must run
// exactly once per frame — a zero dir is what decays the velocity to rest.
fn void camera_update(MovingCamera* camera, math::V2 dir, f32 dt) {
    constexpr f32 CAMERA_SPEED       = 300;  // px/s at full tilt
    constexpr f32 CAMERA_SMOOTH_TIME = 0.1f; // seconds to close ~63% of the velocity gap

    // Exponential smoothing toward the target velocity — framerate
    // independent, and ZII-friendly: no input decays velocity to zero.
    f32 blend = 1 - expf(-dt / CAMERA_SMOOTH_TIME);
    camera->velocity.x += (dir.x * CAMERA_SPEED - camera->velocity.x) * blend;
    camera->velocity.y += (dir.y * CAMERA_SPEED - camera->velocity.y) * blend;
    camera->inner.target.x += camera->velocity.x * dt;
    camera->inner.target.y += camera->velocity.y * dt;
}

// Eases the view onto the followed world position; the same exponential
// smoothing as camera_update, applied to the target instead of the velocity.
fn void camera_follow(MovingCamera* camera, math::V2 target, f32 dt) {
    constexpr f32 FOLLOW_SMOOTH_TIME = 0.25f; // seconds to close ~63% of the gap

    f32 blend = 1 - expf(-dt / FOLLOW_SMOOTH_TIME);
    camera->inner.target.x += (target.x - camera->inner.target.x) * blend;
    camera->inner.target.y += (target.y - camera->inner.target.y) * blend;
}

fn math::Rect camera_view_rect(MovingCamera camera, math::V2 screen_size) {
    math::V2 view_tl = ray::screen_to_world(camera.inner, {0, 0});
    math::V2 view_br = ray::screen_to_world(camera.inner, screen_size);
    return {view_tl.x, view_tl.y, view_br.x - view_tl.x, view_br.y - view_tl.y};
}

fn void draw_board_layer(game::DrawMap draw_map, MovingCamera camera) {
    // World space: the square stays put while the camera pans over it, and
    // the mouse converts through the same camera the drawing uses.
    ray::camera_begin(camera.inner);

    for (const auto& item : draw_map.items) {
        switch (item.style) {
            case game::BodyShape::Rectangle: ray::fill_rect(item.bounds, item.color, 0); break;
            case game::BodyShape::Circle: ray::fill_circle(item.bounds, item.color); break;
        }
    }

    ray::camera_end();
}

// ---------------------------------------------------------------------- ui

// (Re)load the UI definitions, logging whatever is wrong with them. The R
// key calls this again — that is the whole hot reload.
fn void load_ui(ui::Ui* hud) {
    for (String problem : ui::load(hud, "data/ui.txt", "data/style.txt")) {
        LOG("ui: %.*s", (int)problem.len, problem.data);
    }
}

// The interaction defs reload the same way (initialize did the first load).
fn void load_interactions(game::Game* game) {
    for (String problem : game::interactions_load(&game->interactions, "data/interactions.txt")) {
        LOG("interactions: %.*s", (int)problem.len, problem.data);
    }
}

// Placeholder sim state the demo UI drives; a real sim replaces this.
struct TimeState {
    b32 paused;
    i32 speed;
    f32 accum;
};

fn void time_update(TimeState* time, TimeCommand command) {
    time->paused ^= command.toggle_pause;
    if (command.set_speed > 0) {
        time->paused = false;
        time->speed  = min(command.set_speed, MAX_SPEED);
    }
}

// delta is the wall time to convert into day-ticks; the caller owns where it
// comes from — and zeroes it to hold time (ZII: a zero delta accrues nothing).
fn usize time_tick(TimeState* state, f32 delta) {
    state->accum += delta * state->speed * !state->paused;
    usize ticks = (usize)(state->accum);
    state->accum -= ticks;
    return ticks;
}

// The data the script's $VARs bind against, rebuilt every frame from
// whatever state the game has. Formatted strings land in the frame arena.
// A held clock (sim prompt, travel picker) reads as paused and locks every
// time control — the player cannot unpause over the hold.
fn void fill_ui_data(ui::data::Data* data, arena::Arena* frame, const TimeState* time, b32 time_held) {
    b32 paused = time->paused || time_held;
    ui::data::bind_global(data, "TIME_BUTTON", paused ? "Paused" : "Playing");
    ui::data::bind_global(data, "TIME_ENABLED", !time_held);
    // $HAS_PLAYER and $INTERACTION stay unbound: those panels stay hidden.

    // Speed buttons, one list row per level: the current level is the one
    // you can't press, and pause locks them all.
    ui::data::begin_list(data, "speeds");
    for (i32 level = 1; level <= MAX_SPEED; ++level) {
        ui::data::begin_row(data);
        ui::data::bind(data, "LEVEL", string::format(frame, "%d", level));
        ui::data::bind(data, "ENABLED", !paused && time->speed != level);
    }
}

// The travel target picker: one clicked map cell, whose occupants are
// re-listed every frame from the live world.
struct TargetPick {
    b32           open;
    game::CellPos cell;
};

// Binds the picked cell's things as travel targets. The panel hides when
// nothing but the player stands there; ids cross the action wire in
// parse_command's "travel_to = { slot generation }" form. Returns whether
// the panel is actually showing — the caller holds time while it is.
fn b32 fill_target_data(ui::data::Data* data, arena::Arena* frame, const game::Game* game, TargetPick pick) {
    if (!pick.open) return false;
    const game::World* world = &game->world;

    usize count = 0;
    for (const game::Thing* thing : game::occupants_of(&game->spatial, world, pick.cell)) {
        count += thing->id == world->player ? 0 : 1;
    }
    if (!count) return false;

    ui::data::bind_global(data, "TARGETS", true);
    ui::data::begin_list(data, "targets");
    for (const game::Thing* thing : game::occupants_of(&game->spatial, world, pick.cell)) {
        if (thing->id == world->player) continue;
        ui::data::begin_row(data);
        ui::data::bind(data, "TARGET_REF",
                       string::format(frame, "{ %d %d }", (int)thing->id.slot, (int)thing->id.generation));
        ui::data::bind(data, "TARGET_NAME", game::resolve_name(world, thing->name));
    }
    return true;
}

// Turns one interpolated action string into a Command. The action is the
// inside of a block — one record patched onto the Command schema:
// "time_toggle = yes", "time_speed = 3", "choose = 2". Reads are
// config-style declarative: known fields apply, unknown keys are ignored
// (the unconditional action LOG is the tripwire for typos), and on parse
// errors log_msg is set while whatever recovered still applies.
fn Command parse_command(arena::Arena* frame, String action) {
    Command command = {};

    tabula::ParseResult parsed = tabula::parse(frame, action);
    if (parsed.errors.len) {
        command.log_msg = string::format(frame, "garbled action: %.*s", (int)action.len, action.data);
    }

    tabula::Node root = {};
    root.kind         = tabula::Kind::Block;
    root.children     = parsed.roots;

    command.reload_ui         = tabula::read_b32(&root, "reload_ui", false);
    command.time.toggle_pause = tabula::read_b32(&root, "time_toggle", false);
    command.time.set_speed    = tabula::read_i32(&root, "time_speed", 0);
    command.game              = game::parse_command(&root);
    return command;
}

// Executes one frame's draw commands against ray. Commands are in draw
// order with nested clip regions; ray's scissor doesn't nest, so the stack
// lives here and each ClipStart sets the running intersection.
fn void render_ui_commands(Slice<ui::DrawCommand> commands, ray::FontId font) {
    Array<math::Rect, 16> clip_stack = {};
    usize                 clip_count = 0;
    for (const ui::DrawCommand& command : commands) {
        switch (command.kind) {
            case ui::DrawKind::Rectangle: ray::fill_rect(command.bounds, command.color, command.corner_radius); break;
            case ui::DrawKind::Text:
                // ray draws from the top-left of the glyph box; the
                // command's baseline is not needed. Snap the origin to a
                // whole pixel — a fractional start smears every glyph
                // across two pixel rows/columns.
                ray::draw_text(command.text, font, {roundf(command.bounds.x), roundf(command.bounds.y)},
                               (i32)command.text_size, command.color);
                break;
            case ui::DrawKind::Border:
                ray::stroke_rect(command.bounds, command.border_width, command.color, command.corner_radius);
                break;
            case ui::DrawKind::Image:
                // Both handles are the same opaque u64 — ui carries ray's
                // texture key through untouched.
                ray::draw_texture(ray::TextureId{command.image.value}, {}, command.bounds, command.color);
                break;
            case ui::DrawKind::ClipStart: {
                math::Rect clip =
                    clip_count ? math::intersect(clip_stack[clip_count - 1], command.bounds) : command.bounds;
                ASSERT(clip_count < 16);
                clip_stack[clip_count] = clip;
                clip_count += 1;
                ray::clip_begin(clip);
            } break;
            case ui::DrawKind::ClipEnd:
                if (clip_count) clip_count -= 1;
                if (clip_count) {
                    ray::clip_begin(clip_stack[clip_count - 1]);
                } else {
                    ray::clip_end();
                }
                break;
            case ui::DrawKind::Nil: break;
        }
    }
}

int main() {
    // Cleared at the top of every frame: UI bindings, interpolated strings,
    // the frame's ui::Result.
    arena::Arena frame_arena = {};
    arena::reserve(&frame_arena, 16 * MB);

    Config config = load_config(&frame_arena, "data/config.txt");
    ray::window_open(config.screen_width, config.screen_height, "imperium", config.vsync);
    if (!config.vsync) ray::target_fps(60); // CPU-side pacing only when the display isn't doing it

    // Zero Font on failure is fine: text falls back to the default font.
    ray::Font font = ray::load_font_from_file("assets/fonts/default.ttf", 48);

    MovingCamera camera = {};
    // target is pinned at offset: half the screen, so the point the camera
    // holds (the followed player, a pan) sits in the middle, not the corner.
    camera.inner.offset = {(f32)config.screen_width / 2, (f32)config.screen_height / 2};
    // Manual panning switches to free look; F snaps back onto the player.
    b32        free_look = false;
    TargetPick pick      = {};

    ui::Ui hud = {};
    load_ui(&hud);

    TimeState time = {};
    time.speed     = 1;
    time.paused    = true;

    arena::Arena permanent_arena = {};
    arena::reserve(&permanent_arena, 1 * GB);

    game::Game* game = arena::allocate<game::Game>(&permanent_arena);
    game::initialize(&frame_arena, game);

    while (!ray::window_should_close()) {
        arena::reset(&frame_arena);

        auto commands = vec::make_vec<Command>(&frame_arena, 256);
        key_input(&commands);

        // Last tick's verdict: an open interaction holds time for the whole
        // frame — the UI shows the hold, and the delta below zeroes out.
        // The travel picker holds it the same way: both are decisions the
        // world waits on.
        auto ui_data   = game::extract_ui_data(&frame_arena, game);
        b32  picking   = fill_target_data(&ui_data, &frame_arena, game, pick);
        b32  time_held = game::forced_pause(game) || picking;
        fill_ui_data(&ui_data, &frame_arena, &time, time_held);

        ui::Input input     = {};
        input.bounds        = {0, 0, (f32)config.screen_width, (f32)config.screen_height};
        input.mouse_pos     = ray::mouse_pos();
        input.mouse_pressed = ray::mouse_pressed(ray::MouseButton::Left);
        input.mouse_down    = ray::mouse_down(ray::MouseButton::Left);
        // raylib reports the wheel in notches; the engine scrolls in pixels.
        input.wheel = ray::mouse_wheel() * 40;

        auto ui_frame = ui::run(&hud, &frame_arena, input, &ui_data, [&](String text, u16 size) {
            auto measured = ray::measure_text(text, font.id, (i32)size);
            return ui::TextMetrics{measured.size, measured.baseline};
        });
        for (String action : ui_frame.actions) {
            // Unconditional log, so unwired buttons still show life.
            LOG("ui action: %.*s", (int)action.len, action.data);
            vec::push(&commands, parse_command(&frame_arena, action));
        }

        // A world click — one the UI didn't claim — picks a map cell; its
        // occupants become the travel targets shown from next frame on.
        // Clicking bare ground closes the picker.
        if (input.mouse_pressed && !ui_frame.pointer_over_ui) {
            math::V2      clicked = ray::screen_to_world(camera.inner, input.mouse_pos);
            game::CellPos cell    = {(i32)floorf(clicked.x / game::CELL_SIZE + 0.5f),
                                     (i32)floorf(clicked.y / game::CELL_SIZE + 0.5f)};
            pick = {};
            if (game::in_bounds(&game->world.world_map, cell)) {
                pick.open = true;
                pick.cell = cell;
            }
        }

        auto game_commands = vec::make_vec<game::Command>(&frame_arena, 8);

        for (const auto& command : commands) {
            if (command.log_msg.len) LOG("command: %.*s", (int)command.log_msg.len, command.log_msg.data);
            if (command.reload_ui) {
                load_ui(&hud);
                load_interactions(game);
            }
            time_update(&time, command.time);
            if (command.camera_move.x != 0 || command.camera_move.y != 0) free_look = true;
            if (command.follow_player) free_look = false;
            camera_update(&camera, command.camera_move, ray::frame_time());
            if (command.game.kind == game::CommandKind::TravelTo) pick = {}; // the picker served its purpose
            if (command.game.kind != game::CommandKind::Nil) vec::push(&game_commands, command.game);
        }

        if (!free_look) {
            game::Thing* anchor = game::resolve_map_anchor(&game->world, game->world.player);
            if (anchor->id) {
                math::V2 focus = {anchor->cell_pos.x * game::CELL_SIZE, anchor->cell_pos.y * game::CELL_SIZE};
                camera_follow(&camera, focus, ray::frame_time());
            }
        }

        // After camera_update, so the culling rect is this frame's view.
        math::V2 screen_size  = {(f32)config.screen_width, (f32)config.screen_height};
        auto     visible_rect = camera_view_rect(camera, screen_size);
        auto     draw_map     = game::draw_map(&frame_arena, game, visible_rect);

        ray::frame_begin();
        ray::clear({40, 40, 46, 255});
        draw_board_layer(draw_map, camera);
        render_ui_commands(ui_frame.commands, font.id);
        ray::frame_end();

        usize num_days = time_tick(&time, ray::frame_time() * !time_held);

        game::TickCommands tick_commands = {
            .commands = vec::slice(game_commands),
            .num_days = num_days,
        };

        game::tick(game, tick_commands);
    }
    ray::window_close();

    return 0;
}
