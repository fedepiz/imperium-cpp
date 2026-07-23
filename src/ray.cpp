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
// raylib types; outside, only the id keys circulate. Slot budgets are
// per-system constants.
//
// A font entry keeps the font file's bytes and rasterizes one atlas per
// text size on demand — drawing a 48px atlas scaled to 22px looks thin and
// washed out, so every size gets its own crisp rasterization (what
// macroquad did for the Rust build).
constexpr usize MAX_FONTS      = 16;
constexpr usize MAX_FONT_SIZES = 8; // distinct rasterized sizes per font

struct FontEntry {
    u8*    data; // the font file, kept for on-demand rasterization
    i32    data_size;
    char   extension[8]; // ".ttf" / ".otf" — LoadFontFromMemory dispatches on it
    i32    sizes[MAX_FONT_SIZES];
    ::Font fonts[MAX_FONT_SIZES];
    u32    count;
    b32    overflow_logged;
};

pool::Pool<FontId, FontEntry, MAX_FONTS> FONTS;

constexpr usize                                MAX_TEXTURES = 64;
pool::Pool<TextureId, ::Texture, MAX_TEXTURES> TEXTURES;

// ASCII plus Latin-1 supplement, so accented glyphs (the layout engine's
// line probe includes 'Á') rasterize instead of falling back to '?'.
fn ::Font rasterize(const FontEntry* entry, i32 size) {
    int codepoints[224];
    for (int i = 0; i < 224; ++i) codepoints[i] = 32 + i;
    ::Font font = LoadFontFromMemory(entry->extension, entry->data, entry->data_size, size, codepoints, 224);
    if (font.texture.id != 0) SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    return font;
}

// The atlas for this font at exactly this size, rasterizing on first use.
// Nil/stale ids and rasterization failures fall back to the default font.
// A full size budget is a bug in the caller's size discipline (steady-state
// UIs use a handful of role sizes): logged once, then the closest cached
// size stands in — scaled, so visibly degraded, never churning.
fn ::Font font_for(FontId id, i32 size) {
    FontEntry* entry = &FONTS[id]; // nil/stale key -> zeroed dummy -> null data
    if (!entry->data) return GetFontDefault();
    for (u32 i = 0; i < entry->count; ++i) {
        if (entry->sizes[i] == size) return entry->fonts[i];
    }
    if (entry->count == MAX_FONT_SIZES) {
        if (!entry->overflow_logged) {
            LOG("ray: font size budget (", MAX_FONT_SIZES, ") exhausted; ", size, " px text draws scaled");
            entry->overflow_logged = true;
        }
        u32 closest = 0;
        i32 best    = 0x7fffffff;
        for (u32 i = 0; i < entry->count; ++i) {
            i32 distance = entry->sizes[i] > size ? entry->sizes[i] - size : size - entry->sizes[i];
            if (distance < best) {
                best    = distance;
                closest = i;
            }
        }
        return entry->fonts[closest];
    }
    ::Font font = rasterize(entry, size);
    if (font.texture.id == 0) return GetFontDefault();
    entry->sizes[entry->count] = size;
    entry->fonts[entry->count] = font;
    entry->count += 1;
    return font;
}

} // namespace

void window_open(i32 width, i32 height, String title, b32 vsync) {
    if (vsync) SetConfigFlags(FLAG_VSYNC_HINT); // must precede InitWindow
    char buffer[256];
    InitWindow(width, height, to_cstr(title, buffer, sizeof(buffer)));
}

Font load_font_from_file(String path, i32 size) {
    char        buffer[256];
    const char* cpath     = to_cstr(path, buffer, sizeof(buffer));
    int         data_size = 0;
    u8*         data      = LoadFileData(cpath, &data_size);
    if (!data || data_size <= 0) return {}; // unreadable file (ZII)

    FontEntry entry = {};
    entry.data      = data;
    entry.data_size = data_size;
    const char* extension = strrchr(cpath, '.');
    if (!extension) extension = ".ttf";
    for (usize i = 0; i + 1 < sizeof(entry.extension) && extension[i]; ++i) entry.extension[i] = extension[i];

    // Rasterize the requested size now — it validates the file, and it is
    // the size the caller is about to use anyway.
    ::Font first = rasterize(&entry, size);
    if (first.texture.id == 0) {
        UnloadFileData(data);
        return {}; // not a font raylib can parse (ZII)
    }
    entry.sizes[0] = size;
    entry.fonts[0] = first;
    entry.count    = 1;

    FontId id = pool::insert(&FONTS, entry);
    if (pool::is_nil_key(id)) { // store full — degrade to the default font
        UnloadFont(first);
        UnloadFileData(data);
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
    for (auto& entry : FONTS) {
        for (u32 i = 0; i < entry.value.count; ++i) UnloadFont(entry.value.fonts[i]);
        UnloadFileData(entry.value.data);
    }
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
    // The stroke sits inside rect — borders are inset into the bounding box,
    // so a border never spills outside its element's bounds.
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

void fill_circle(Rect bounds, Color color) {
    V2  center = {bounds.x + bounds.w / 2, bounds.y + bounds.h / 2};
    f32 radius = (bounds.w < bounds.h ? bounds.w : bounds.h) / 2;
    DrawCircleV(to_raylib(center), radius, to_raylib(color));
}

void draw_text(String text, FontId font_id, V2 pos, i32 size, Color color) {
    char   buffer[1024];
    ::Font font = font_for(font_id, size); // an atlas rasterized at exactly this size
    // size/10 spacing matches what raylib's own DrawText uses internally.
    DrawTextEx(font, to_cstr(text, buffer, sizeof(buffer)), to_raylib(pos), (f32)size, (f32)size / 10,
               to_raylib(color));
}

TextMetrics measure_text(String text, FontId font_id, i32 size) {
    char   buffer[1024];
    ::Font font = font_for(font_id, size); // the same atlas draw_text uses
    // Same spacing as draw_text, so measure and draw agree.
    ::Vector2 measured = MeasureTextEx(font, to_cstr(text, buffer, sizeof(buffer)), (f32)size, (f32)size / 10);
    // raylib draws from the top-left, so the baseline is cosmetic here; the
    // glyph boxes span the full line height, making the box bottom the best
    // stand-in for consumers that want one.
    TextMetrics result = {};
    result.size        = {measured.x, measured.y};
    result.baseline    = measured.y;
    return result;
}

void clip_begin(Rect rect) {
    BeginScissorMode((int)rect.x, (int)rect.y, (int)rect.w, (int)rect.h);
}

void clip_end() { EndScissorMode(); }

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

V2 mouse_wheel() {
    ::Vector2 wheel = GetMouseWheelMoveV();
    return {wheel.x, wheel.y};
}
} // namespace ray
