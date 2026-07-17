#include "ray.hpp"
#include "pool.hpp"
#include <raylib.h>

// The one TU that sees raylib.h (see ray.hpp). In here raylib's Color is
// ::Color and ours is ray::Color; raylib's color macros (WHITE, RED, ...) are
// live past the include, so never spell ray::WHITE etc. in this file.

// Key values are raylib's; assert the endpoints of each contiguous run.
static_assert((u32)ray::Key::Space == KEY_SPACE);
static_assert((u32)ray::Key::Comma == KEY_COMMA);
static_assert((u32)ray::Key::Period == KEY_PERIOD);
static_assert((u32)ray::Key::Zero == KEY_ZERO);
static_assert((u32)ray::Key::Nine == KEY_NINE);
static_assert((u32)ray::Key::Equal == KEY_EQUAL);
static_assert((u32)ray::Key::A == KEY_A);
static_assert((u32)ray::Key::Z == KEY_Z);
static_assert((u32)ray::Key::Escape == KEY_ESCAPE);
static_assert((u32)ray::Key::Delete == KEY_DELETE);
static_assert((u32)ray::Key::Right == KEY_RIGHT);
static_assert((u32)ray::Key::Up == KEY_UP);
static_assert((u32)ray::Key::PageUp == KEY_PAGE_UP);
static_assert((u32)ray::Key::End == KEY_END);
static_assert((u32)ray::Key::F1 == KEY_F1);
static_assert((u32)ray::Key::F12 == KEY_F12);
static_assert((u32)ray::Key::LeftShift == KEY_LEFT_SHIFT);
static_assert((u32)ray::Key::RightSuper == KEY_RIGHT_SUPER);

namespace ray {

namespace {

::Color to_raylib(Color color) { return {color.r, color.g, color.b, color.a}; }

int to_raylib(MouseButton button) {
    ASSERT(button != MouseButton::Nil);
    return (int)button - 1; // raylib: MOUSE_BUTTON_LEFT = 0
}

// Boundary conversion: raylib wants null-terminated strings. Truncates to the
// buffer, so callers pick the budget via buffer size.
const char* to_cstr(String text, char* buffer, usize capacity) {
    usize count = text.len < capacity - 1 ? text.len : capacity - 1;
    if (count) memcpy(buffer, text.data, count);
    buffer[count] = 0;
    return buffer;
}

// The centralized font store. Inside this TU the values are raylib ::Font;
// outside, only FontId keys circulate. Slot budget is a per-system constant.
constexpr usize MAX_FONTS = 64;
pool::Pool<FontId, ::Font, MAX_FONTS> FONTS;

} // namespace

void window_open(i32 width, i32 height, String title, b32 vsync) {
    if (vsync) SetConfigFlags(FLAG_VSYNC_HINT); // must precede InitWindow
    char buffer[256];
    InitWindow(width, height, to_cstr(title, buffer, sizeof(buffer)));
}

Font load_font_from_file(String path, i32 size) {
    char   buffer[256];
    ::Font loaded = LoadFontEx(to_cstr(path, buffer, sizeof(buffer)), size, 0, 0);
    // raylib hands back the shared default font on failure; that one is not
    // ours to store or unload — report the load as failed instead (ZII).
    if (loaded.texture.id == 0 || loaded.texture.id == GetFontDefault().texture.id) return {};

    FontId id = pool::insert(&FONTS, loaded);
    if (pool::is_nil_key(id)) { // store full — degrade to the default font
        UnloadFont(loaded);
        return {};
    }

    Font result = {};
    result.id   = id;
    result.size = size;
    return result;
}

void window_close() {
    for (auto& entry : FONTS) UnloadFont(entry.value);
    memset(&FONTS, 0, sizeof(FONTS)); // ZII reset: every outstanding FontId is now stale
    CloseWindow();
}
bool window_should_close() { return WindowShouldClose(); }
void target_fps(i32 fps) { SetTargetFPS(fps); }

void frame_begin() { BeginDrawing(); }
void frame_end() { EndDrawing(); }
void clear(Color color) { ClearBackground(to_raylib(color)); }
f32  frame_time() { return GetFrameTime(); }

void draw_rect(f32 x, f32 y, f32 width, f32 height, Color color) {
    DrawRectangleRec({x, y, width, height}, to_raylib(color));
}

void draw_text(String text, FontId font_id, i32 x, i32 y, i32 size, Color color) {
    char   buffer[1024];
    ::Font font = FONTS[font_id]; // nil/stale key -> zeroed dummy -> default font
    if (font.texture.id == 0) font = GetFontDefault();
    // size/10 spacing matches what raylib's own DrawText uses internally.
    DrawTextEx(font, to_cstr(text, buffer, sizeof(buffer)), {(f32)x, (f32)y}, (f32)size, (f32)size / 10,
               to_raylib(color));
}

bool key_down(Key key) { return IsKeyDown((int)key); }
bool key_pressed(Key key) { return IsKeyPressed((int)key); }
bool key_released(Key key) { return IsKeyReleased((int)key); }
bool mouse_down(MouseButton button) { return IsMouseButtonDown(to_raylib(button)); }
bool mouse_pressed(MouseButton button) { return IsMouseButtonPressed(to_raylib(button)); }
f32  mouse_x() { return GetMousePosition().x; }
f32  mouse_y() { return GetMousePosition().y; }

} // namespace ray
