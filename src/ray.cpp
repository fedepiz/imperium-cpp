#include "ray.hpp"
#include "pool.hpp"
#include <raylib.h>
#include <math.h>

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

// Filter values are raylib's, so the boundary is a cast.
static_assert((u32)ray::TextureFilter::Point == TEXTURE_FILTER_POINT);
static_assert((u32)ray::TextureFilter::Bilinear == TEXTURE_FILTER_BILINEAR);
static_assert((u32)ray::TextureFilter::Trilinear == TEXTURE_FILTER_TRILINEAR);

namespace ray {

namespace {

// Boundary-TU exception to the fn rule: only these internal helpers are fn —
// the ray.hpp API implementations below stay plain, keeping the one strong
// definition every other TU links against.
fn ::Color to_raylib(Color color) { return {color.r, color.g, color.b, color.a}; }

fn ::Rectangle to_raylib(Rect rect) { return {rect.x, rect.y, rect.w, rect.h}; }

fn ::Vector2 to_raylib(V2 v) { return {v.x, v.y}; }

fn ::Camera2D to_raylib(Camera camera) {
    ::Camera2D result = {};
    result.target     = to_raylib(camera.target);
    result.offset     = to_raylib(camera.offset);
    result.rotation   = camera.rotation;
    result.zoom       = camera.zoom == 0 ? 1 : camera.zoom; // ZII: zoom 0 draws as 1
    return result;
}

fn int to_raylib(MouseButton button) {
    ASSERT(button != MouseButton::Nil);
    return (int)button - 1; // raylib: MOUSE_BUTTON_LEFT = 0
}

// Boundary conversion: raylib wants null-terminated strings. Truncates to the
// buffer, so callers pick the budget via buffer size.
fn const char* to_cstr(String text, char* buffer, usize capacity) {
    usize count = text.len < capacity - 1 ? text.len : capacity - 1;
    if (count) memcpy(buffer, text.data, count);
    buffer[count] = 0;
    return buffer;
}

// The centralized font and texture stores. Inside this TU the values are
// raylib ::Font / ::Texture; outside, only the id keys circulate. Slot
// budgets are per-system constants.
constexpr usize                       MAX_FONTS = 64;
pool::Pool<FontId, ::Font, MAX_FONTS> FONTS;

constexpr usize                                MAX_TEXTURES = 64;
pool::Pool<TextureId, ::Texture, MAX_TEXTURES> TEXTURES;

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

Texture load_texture_from_file(String path, TextureFilter filter) {
    char      buffer[256];
    ::Texture loaded = LoadTexture(to_cstr(path, buffer, sizeof(buffer)));
    if (loaded.id == 0) return {}; // load failed (ZII)

    // Trilinear samples between mip levels, so the chain must exist; the
    // other filters run on the base level alone.
    if (filter == TextureFilter::Trilinear) GenTextureMipmaps(&loaded);
    SetTextureFilter(loaded, (int)filter);

    TextureId id = pool::insert(&TEXTURES, loaded);
    if (pool::is_nil_key(id)) { // store full
        UnloadTexture(loaded);
        return {};
    }

    Texture result = {};
    result.id      = id;
    result.width   = (u16)loaded.width;
    result.height  = (u16)loaded.height;
    return result;
}

void window_close() {
    for (auto& entry : FONTS)
        UnloadFont(entry.value);
    memset(&FONTS, 0, sizeof(FONTS)); // ZII reset: every outstanding FontId is now stale
    for (auto& entry : TEXTURES)
        UnloadTexture(entry.value);
    memset(&TEXTURES, 0, sizeof(TEXTURES));
    CloseWindow();
}
b32  window_should_close() { return WindowShouldClose(); }
void target_fps(i32 fps) { SetTargetFPS(fps); }

void frame_begin() { BeginDrawing(); }
void frame_end() { EndDrawing(); }
void clear(Color color) { ClearBackground(to_raylib(color)); }
f32  frame_time() { return GetFrameTime(); }

void camera_begin(Camera camera) { BeginMode2D(to_raylib(camera)); }
void camera_end() { EndMode2D(); }

V2 screen_to_world(Camera camera, V2 screen) {
    ::Vector2 world = GetScreenToWorld2D(to_raylib(screen), to_raylib(camera));
    return {world.x, world.y};
}

V2 world_to_screen(Camera camera, V2 world) {
    ::Vector2 screen = GetWorldToScreen2D(to_raylib(world), to_raylib(camera));
    return {screen.x, screen.y};
}

namespace {

// Corner tessellation for rounded rects, shared by fill and stroke. Left to
// auto-pick, raylib chooses different segment counts for the two, so the
// fill's coarser corner polygon pokes past the stroke's chords; one count
// computed from the outer radius (raylib's own error-rate formula,
// SMOOTH_CIRCLE_ERROR_RATE = 0.5) makes both walk identical vertices.
fn int corner_segments(f32 radius) {
    if (radius < 1) return 4;
    f32 x        = 1 - 0.5f / radius;
    int segments = (int)(ceilf(2 * PI / acosf(2 * x * x - 1)) / 4.0f);
    return segments < 4 ? 4 : segments;
}

} // namespace

void fill_rect(Rect rect, Color color, f32 corner_radius) {
    if (corner_radius <= 0) {
        DrawRectangleRec(to_raylib(rect), to_raylib(color));
        return;
    }
    // raylib takes roundness relative to the short side: radius = min(w,h)/2
    // at 1.0 (it clamps above that).
    f32 short_side = rect.w < rect.h ? rect.w : rect.h;
    DrawRectangleRounded(to_raylib(rect), 2 * corner_radius / short_side, corner_segments(corner_radius),
                         to_raylib(color));
}

void stroke_rect(Rect rect, f32 thickness, Color color, f32 corner_radius) {
    // The stroke sits inside rect — Clay's border convention ("inset into the
    // bounding box"), so these strokes line up with Clay borders later.
    // radius <= thickness leaves no room for an inner arc: square corners.
    if (corner_radius <= thickness) {
        DrawRectangleLinesEx(to_raylib(rect), thickness, to_raylib(color)); // draws inset already
        return;
    }
    // The rounded variant instead rings *outward* from the rect it is given,
    // and nudges every coordinate half a pixel inward: pass the rect shrunk
    // by thickness - 0.5 with the radius reduced by thickness, and the outer
    // edge lands exactly on rect with the requested corner radius — the same
    // vertices fill_rect walks, given the shared segment count.
    f32  inset      = thickness - 0.5f;
    Rect inner      = {rect.x + inset, rect.y + inset, rect.w - 2 * inset, rect.h - 2 * inset};
    f32  short_side = inner.w < inner.h ? inner.w : inner.h;
    DrawRectangleRoundedLinesEx(to_raylib(inner), 2 * (corner_radius - thickness) / short_side,
                                corner_segments(corner_radius), thickness, to_raylib(color));
}

void draw_text(String text, FontId font_id, V2 pos, i32 size, Color color) {
    char   buffer[1024];
    ::Font font = FONTS[font_id]; // nil/stale key -> zeroed dummy -> default font
    if (font.texture.id == 0) font = GetFontDefault();
    // size/10 spacing matches what raylib's own DrawText uses internally.
    DrawTextEx(font, to_cstr(text, buffer, sizeof(buffer)), to_raylib(pos), (f32)size, (f32)size / 10,
               to_raylib(color));
}

void draw_texture(TextureId texture_id, Rect source, Rect dest, Color color) {
    ::Texture texture = TEXTURES[texture_id]; // nil/stale key -> zeroed dummy
    if (texture.id == 0) return;              // no default texture to fall back to — draw nothing
    // ZII: the zero source rect means the whole texture.
    if (source == Rect{}) source = {0, 0, (f32)texture.width, (f32)texture.height};
    DrawTexturePro(texture, to_raylib(source), to_raylib(dest), {0, 0}, 0, to_raylib(color));
}

b32 key_down(Key key) { return IsKeyDown((int)key); }
b32 key_pressed(Key key) { return IsKeyPressed((int)key); }
b32 key_released(Key key) { return IsKeyReleased((int)key); }
b32 mouse_down(MouseButton button) { return IsMouseButtonDown(to_raylib(button)); }
b32 mouse_pressed(MouseButton button) { return IsMouseButtonPressed(to_raylib(button)); }

V2 mouse_pos() {
    ::Vector2 pos = GetMousePosition();
    return {pos.x, pos.y};
}
} // namespace ray
