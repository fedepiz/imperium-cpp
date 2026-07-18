#include "core.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "math.hpp"
#include "tabula.hpp"
#include "ray.hpp"
#include <iostream>
#include <math.h>

// String is not null-terminated — print by explicit length. Lives here because
// iostream is root-only; modules never see it.
fn std::ostream& operator<<(std::ostream& out, String string) {
    return out.write(string.data, (std::streamsize)string.len);
}

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

struct Ui {
    String    name;
    ray::Font font;
};

struct Board {
    MovingCamera camera;
    math::V2     square_pos; // world-space center of the toggle square
    b32          toggle;
};

fn void draw_board_layer(Board* board, MovingCamera camera) {

    // World space: the square stays put while the camera pans over it, and
    // the mouse converts through the same camera the drawing uses.
    ray::camera_begin(camera.inner);
    {
        auto bounds = math::rect_with_center_and_size(board->square_pos, math::splat(64));
        auto fill   = board->toggle ? ray::BLUE : ray::RED;
        ray::fill_rect(bounds, fill, 8);
        ray::stroke_rect(bounds, 4, ray::GREEN, 8);
        auto mouse_world = ray::screen_to_world(camera.inner, ray::mouse_pos());
        if (math::contains(bounds, mouse_world) && ray::mouse_pressed(ray::MouseButton::Left)) {
            board->toggle ^= true;
        }
    }
    ray::camera_end();
}

fn void draw_ui_layer(Ui* ui) { ray::draw_text(ui->name, ui->font.id, {32, 32}, ui->font.size, ray::WHITE); }

int main() {
    arena::Arena arena;
    arena::reserve(&arena, 64 * 1024 * 1024);

    auto source = file_io::read_file_to_string(&arena, "data/example.txt");
    for (auto error : source.messages) {
        std::cout << error.message << std::endl;
    }
    auto parsed = tabula::parse(&arena, source.data);
    for (auto error : parsed.errors) {
        std::cout << error.message << std::endl;
    }

    Config config = load_config(&arena, "data/config.txt");
    ray::window_open(config.screen_width, config.screen_height, "imperium", config.vsync);
    if (!config.vsync) ray::target_fps(60); // CPU-side pacing only when the display isn't doing it

    // Zero Font on failure is fine: draw_text falls back to the default font.
    ray::Font font = ray::load_font_from_file("assets/fonts/default.ttf", 48);

    Board board      = {};
    board.square_pos = {100, 100};

    Ui ui   = {};
    ui.name = parsed.roots.len ? tabula::get_text(&parsed.roots[0], "id") : String{};
    ui.font = font;

    while (!ray::window_should_close()) {
        camera_update(&board.camera, ray::frame_time());

        ray::frame_begin();
        ray::clear({40, 40, 46, 255});
        draw_board_layer(&board, board.camera);
        draw_ui_layer(&ui);
        ray::frame_end();
    }
    ray::window_close();

    return 0;
}
