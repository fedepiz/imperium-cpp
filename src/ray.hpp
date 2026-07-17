#pragma once
#include "core.hpp"

// Boundary wrapper around raylib — the one sanctioned break in the unity
// build. raylib's header declares unprefixed C names (Color, Rectangle,
// DrawText, the color macros, ...), so it never enters our TUs: this module
// is declaration-only, and ray.cpp — the single TU that includes raylib.h —
// implements it, converting types at the boundary. build.sh compiles ray.cpp
// once to build/ray.o and links it into every binary.

namespace ray {

// ZII: zero is transparent black.
struct Color {
    u8 r, g, b, a;
};

inline constexpr Color BLACK     = {0, 0, 0, 255};
inline constexpr Color WHITE     = {255, 255, 255, 255};
inline constexpr Color RED       = {230, 41, 55, 255};
inline constexpr Color GREEN     = {0, 228, 48, 255};
inline constexpr Color BLUE      = {0, 121, 241, 255};
inline constexpr Color DARK_GRAY = {80, 80, 80, 255};

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

// Key into the module's font store — pool-key conformant ({slot, generation},
// zero = nil). Drawing with a nil or stale key falls back to raylib's
// built-in default font, so a FontId is always safe to use.
struct FontId {
    u32 slot;
    u32 generation;
};

// What loading returns: the store key plus the pixel size the font was
// rasterized at. ZII: the zero Font means "no font" and draws as the default.
struct Font {
    FontId id;
    i32    size;
};

// Rasterizes the file at the given pixel size into the font store. Zero Font
// when the file can't be read or the store is full.
Font load_font_from_file(String path, i32 size);

// Window. Open before any other call. The title is converted to a C string at
// the boundary (truncated past 255 bytes). vsync syncs the buffer swap to the
// display (no tearing) and paces the frame loop to the refresh rate; it is a
// creation-time choice, fixed for the window's lifetime.
void window_open(i32 width, i32 height, String title, b32 vsync);
void window_close();
bool window_should_close(); // close button, or Escape (raylib's default exit key)
void target_fps(i32 fps);

// Frame. Draw calls are valid only between frame_begin and frame_end.
void frame_begin();
void frame_end();
void clear(Color color);
f32  frame_time(); // seconds spent on the last frame

// Drawing. Pixel coordinates, origin top-left.
void draw_rect(f32 x, f32 y, f32 width, f32 height, Color color);
void draw_text(String text, FontId font, i32 x, i32 y, i32 size, Color color); // truncated past 1023 bytes

// Input.
bool key_down(Key key);     // held right now
bool key_pressed(Key key);  // went down this frame
bool key_released(Key key); // went up this frame
bool mouse_down(MouseButton button);
bool mouse_pressed(MouseButton button);
f32  mouse_x();
f32  mouse_y();

} // namespace ray
