#include "core.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "math.hpp"
#include "string.hpp"
#include "tabula.hpp"
#include "ray.hpp"
#include "ui/ui.hpp"
#include <math.h>

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

fn void camera_update(MovingCamera* camera, f32 dt) {
    constexpr f32 CAMERA_SPEED       = 300;  // px/s at full tilt
    constexpr f32 CAMERA_SMOOTH_TIME = 0.1f; // seconds to close ~63% of the velocity gap

    f32 dir_x = 0;
    f32 dir_y = 0;
    if (ray::key_down(ray::Key::Left) || ray::key_down(ray::Key::A)) dir_x -= 1;
    if (ray::key_down(ray::Key::Right) || ray::key_down(ray::Key::D)) dir_x += 1;
    if (ray::key_down(ray::Key::Up) || ray::key_down(ray::Key::W)) dir_y -= 1;
    if (ray::key_down(ray::Key::Down) || ray::key_down(ray::Key::S)) dir_y += 1;
    f32 dir_len = sqrtf(dir_x * dir_x + dir_y * dir_y);
    if (dir_len > 0) { // normalize so diagonals aren't faster
        dir_x /= dir_len;
        dir_y /= dir_len;
    }

    // Exponential smoothing toward the target velocity — framerate
    // independent, and ZII-friendly: no input decays velocity to zero.
    f32 blend = 1 - expf(-dt / CAMERA_SMOOTH_TIME);
    camera->velocity.x += (dir_x * CAMERA_SPEED - camera->velocity.x) * blend;
    camera->velocity.y += (dir_y * CAMERA_SPEED - camera->velocity.y) * blend;
    camera->inner.target.x += camera->velocity.x * dt;
    camera->inner.target.y += camera->velocity.y * dt;
}

struct Board {
    MovingCamera camera;
    math::V2     square_pos; // world-space center of the toggle square
    b32          toggle;
};

fn void draw_board_layer(Board* board, MovingCamera camera, b32 allow_click) {
    // World space: the square stays put while the camera pans over it, and
    // the mouse converts through the same camera the drawing uses.
    ray::camera_begin(camera.inner);
    {
        auto bounds = math::rect_with_center_and_size(board->square_pos, math::splat(64));
        auto fill   = board->toggle ? ray::BLUE : ray::RED;
        ray::fill_rect(bounds, fill, 8);
        ray::stroke_rect(bounds, 4, ray::GREEN, 8);
        auto mouse_world = ray::screen_to_world(camera.inner, ray::mouse_pos());
        if (allow_click && math::contains(bounds, mouse_world) && ray::mouse_pressed(ray::MouseButton::Left)) {
            board->toggle ^= true;
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

// Placeholder sim state the demo UI drives; a real sim replaces this.
struct TimeState {
    b32 paused;
    i32 speed;
};

constexpr i32 MAX_SPEED = 5;

// The data the script's $VARs bind against, rebuilt every frame from
// whatever state the game has. Formatted strings land in the frame arena.
fn void fill_ui_data(ui::data::Data* data, arena::Arena* frame, const TimeState* time, const Board* board) {
    ui::data::bind_global(data, "STATUS", string::format(frame, "%s square", board->toggle ? "blue" : "red"));
    ui::data::bind_global(data, "DATE", "Year 700");
    ui::data::bind_global(data, "TIME_BUTTON", time->paused ? "Paused" : "Playing");
    ui::data::bind_global(data, "TIME_ENABLED", "yes");
    // $HAS_PLAYER and $INTERACTION stay unbound: those panels stay hidden.

    // Speed buttons, one list row per level: the current level is the one
    // you can't press, and pause locks them all.
    ui::data::begin_list(data, "speeds");
    for (i32 level = 1; level <= MAX_SPEED; ++level) {
        ui::data::begin_row(data);
        ui::data::bind(data, "LEVEL", string::format(frame, "%d", level));
        ui::data::bind(data, "ENABLED", (time->paused || time->speed == level) ? "no" : "yes");
    }
}

// Applies one clicked action string to the placeholder state. Every action
// is logged, so unwired buttons still show life.
fn void apply_ui_action(TimeState* time, String action) {
    LOG("ui action: %.*s", (int)action.len, action.data);
    if (string::equals(action, "time_toggle")) {
        time->paused = !time->paused;
        return;
    }
    String speed_prefix = "time_speed ";
    if (string::starts_with(action, speed_prefix)) {
        String                 rest   = {action.len - speed_prefix.len, action.data + speed_prefix.len};
        string::ParseF32Result parsed = string::parse_f32(rest);
        if (parsed.ok) {
            time->speed  = (i32)parsed.value;
            time->paused = false;
        }
    }
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
    arena::Arena arena;
    arena::reserve(&arena, 64 * 1024 * 1024);
    // Cleared at the top of every frame: UI bindings, interpolated strings,
    // the frame's ui::Result.
    arena::Arena frame_arena = {};
    arena::reserve(&frame_arena, 16 * 1024 * 1024);

    Config config = load_config(&arena, "data/config.txt");
    ray::window_open(config.screen_width, config.screen_height, "imperium", config.vsync);
    if (!config.vsync) ray::target_fps(60); // CPU-side pacing only when the display isn't doing it

    // Zero Font on failure is fine: text falls back to the default font.
    ray::Font font = ray::load_font_from_file("assets/fonts/default.ttf", 48);

    Board board      = {};
    board.square_pos = {100, 100};

    ui::Ui hud = {};
    load_ui(&hud);
    TimeState time = {};
    time.speed     = 1;

    while (!ray::window_should_close()) {
        arena::reset(&frame_arena);
        if (ray::key_pressed(ray::Key::R)) load_ui(&hud);
        camera_update(&board.camera, ray::frame_time());

        auto ui_data = ui::data::make(&frame_arena);
        fill_ui_data(&ui_data, &frame_arena, &time, &board);

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
        for (String action : ui_frame.actions)
            apply_ui_action(&time, action);

        ray::frame_begin();
        ray::clear({40, 40, 46, 255});
        draw_board_layer(&board, board.camera, !ui_frame.pointer_over_ui);
        render_ui_commands(ui_frame.commands, font.id);
        ray::frame_end();
    }
    ray::window_close();

    return 0;
}
