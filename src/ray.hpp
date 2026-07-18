#pragma once
#include "core.hpp"
#include "math.hpp"

// Boundary wrapper around raylib — the one sanctioned break in the unity
// build. raylib's header declares unprefixed C names (Color, Rectangle,
// DrawText, the color macros, ...), so it never enters our TUs: this module
// is declaration-only, and ray.cpp — the single TU that includes raylib.h —
// implements it, converting types at the boundary. x.sh compiles ray.cpp
// once to build/ray.o and links it into every binary.

namespace ray {
using namespace math; // Color and the color constants live in math.hpp; ray::Color still spells.
// The using-declaration (unlike the directive) makes Color a member of ray,
// so inside ray.cpp it shadows raylib's global ::Color instead of clashing.
using math::Color;

// Values match raylib's KeyboardKey (GLFW key codes) so the boundary is a
// cast; ray.cpp static_asserts the run endpoints. Zero = no key (KEY_NULL).
enum class Key : u32 {
    Nil   = 0,
    Space = 32,
    Comma = 44,
    Minus,
    Period,
    Zero = 48,
    One,
    Two,
    Three,
    Four,
    Five,
    Six,
    Seven,
    Eight,
    Nine,
    Equal = 61,
    A     = 65,
    B,
    C,
    D,
    E,
    F,
    G,
    H,
    I,
    J,
    K,
    L,
    M,
    N,
    O,
    P,
    Q,
    R,
    S,
    T,
    U,
    V,
    W,
    X,
    Y,
    Z,
    Escape = 256,
    Enter,
    Tab,
    Backspace,
    Insert,
    Delete,
    Right = 262,
    Left,
    Down,
    Up,
    PageUp = 266,
    PageDown,
    Home,
    End,
    F1 = 290,
    F2,
    F3,
    F4,
    F5,
    F6,
    F7,
    F8,
    F9,
    F10,
    F11,
    F12,
    LeftShift = 340,
    LeftControl,
    LeftAlt,
    LeftSuper,
    RightShift = 344,
    RightControl,
    RightAlt,
    RightSuper,
};

enum class MouseButton : u32 {
    Nil, // zero — ZII; asserted against at the boundary
    Left,
    Right,
    Middle,
};

// Opaque key into the module's font store; zero = nil. Drawing with a nil
// or stale key falls back to raylib's built-in default font, so a FontId is
// always safe to use.
struct FontId {
    u64 value;
};

// What loading returns: the store key plus the pixel size rasterized at
// load. ZII: the zero Font means "no font" and draws as the default.
struct Font {
    FontId id;
    i32    size;
};

// Loads the font file into the store and rasterizes it at the given pixel
// size. The store keeps the file's bytes: drawing or measuring at any other
// size rasterizes that size on first use, so text is always drawn from an
// atlas of exactly its own size, never scaled. Zero Font when the file
// can't be read or parsed, or the store is full.
Font load_font_from_file(String path, i32 size);

// Opaque key into the module's texture store; zero = nil. Drawing with a
// nil or stale key draws nothing.
struct TextureId {
    u64 value;
};

struct Texture {
    TextureId id;
    u16 width;
    u16 height;
};

enum class TextureFilter : u8 {
    Point,
    Bilinear,
    Trilinear,
};

Texture load_texture_from_file(String path, TextureFilter filter);

// Window. Open before any other call. The title is converted to a C string at
// the boundary (truncated past 255 bytes). vsync syncs the buffer swap to the
// display (no tearing) and paces the frame loop to the refresh rate; it is a
// creation-time choice, fixed for the window's lifetime.
void window_open(i32 width, i32 height, String title, b32 vsync);
void window_close();
b32 window_should_close(); // close button, or Escape (raylib's default exit key)
void target_fps(i32 fps);

// Frame. Draw calls are valid only between frame_begin and frame_end.
void frame_begin();
void frame_end();
void clear(Color color);
f32  frame_time(); // seconds spent on the last frame

// 2D camera, mirroring raylib's Camera2D: target is the world point pinned at
// offset (screen pixels from the top-left), rotation is in degrees. ZII: the
// zero camera is the identity view — world coordinates are screen
// coordinates; zoom 0 draws as 1 (zero means default, not a collapsed view).
struct Camera {
    V2  target;
    V2  offset;
    f32 rotation;
    f32 zoom;
};

// World-space drawing: draw calls between camera_begin and camera_end take
// world coordinates. The pair nests inside frame_begin/frame_end; anything
// drawn after camera_end is back in screen space (UI).
void camera_begin(Camera camera);
void camera_end();

V2 screen_to_world(Camera camera, V2 screen);
V2 world_to_screen(Camera camera, V2 world);

// Drawing. Pixel coordinates, origin top-left.
void fill_rect(Rect rect, Color color, f32 corner_radius);
void stroke_rect(Rect rect, f32 thickness, Color color, f32 corner_radius);

void draw_text(String text, FontId font, V2 pos, i32 size, Color color); // truncated past 1023 bytes

// Ink extents of text as draw_text would render it (same font fallback, same
// spacing). baseline is the distance from the top of the draw box to the
// baseline — raylib draws from the top-left, so drawing needs no baseline;
// it exists for layout consumers that align text by baseline.
struct TextMetrics {
    V2  size;
    f32 baseline;
};
TextMetrics measure_text(String text, FontId font, i32 size); // truncated past 1023 bytes

// Scissor clipping: draw calls between clip_begin and clip_end only touch
// pixels inside rect. Does not nest at this level — each clip_begin replaces
// the active scissor; callers wanting a stack intersect rects themselves.
void clip_begin(Rect rect);
void clip_end();

void draw_texture(TextureId texture, Rect source, Rect dest, Color color);

// Input.
b32 key_down(Key key);     // held right now
b32 key_pressed(Key key);  // went down this frame
b32 key_released(Key key); // went up this frame
b32 mouse_down(MouseButton button);
b32 mouse_pressed(MouseButton button);
V2 mouse_pos();
V2 mouse_wheel(); // this frame's wheel movement; x is horizontal (trackpads/tilt wheels)
} // namespace ray
