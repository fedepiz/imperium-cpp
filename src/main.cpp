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
std::ostream& operator<<(std::ostream& out, String string) {
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

Config default_config() {
    Config result        = {};
    result.screen_width  = 1280;
    result.screen_height = 720;
    result.vsync         = true;
    return result;
}

Config load_config(arena::Arena* arena, String path) {
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

// Smooth-scrolling camera: velocity eases toward the keyed direction and
// integrates into position. ZII: the zero camera sits at the origin, at rest.
struct Camera {
    math::V2 position;
    math::V2 velocity;
};

constexpr f32 CAMERA_SPEED       = 300;  // px/s at full tilt
constexpr f32 CAMERA_SMOOTH_TIME = 0.1f; // seconds to close ~63% of the velocity gap

void camera_update(Camera* camera, f32 dt) {
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
    camera->position.x += camera->velocity.x * dt;
    camera->position.y += camera->velocity.y * dt;
}

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
    String id = parsed.roots.len ? tabula::get_text(&parsed.roots[0], "id") : String{};

    Config config = load_config(&arena, "data/config.txt");
    ray::window_open(config.screen_width, config.screen_height, "imperium", config.vsync);
    if (!config.vsync) ray::target_fps(60); // CPU-side pacing only when the display isn't doing it

    // Zero Font on failure is fine: draw_text falls back to the default font.
    ray::Font font = ray::load_font_from_file("assets/fonts/default.ttf", 48);

    Camera camera   = {};
    camera.position = {100, 100};
    while (!ray::window_should_close()) {
        camera_update(&camera, ray::frame_time());

        ray::frame_begin();
        ray::clear({40, 40, 46, 255});
        ray::draw_text(id, font.id, 32, 32, font.size, ray::WHITE);
        ray::draw_rect(camera.position.x, camera.position.y, 64, 64, ray::RED);
        ray::frame_end();
    }
    ray::window_close();

    return 0;
}
