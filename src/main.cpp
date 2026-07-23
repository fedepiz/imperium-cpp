#include "core.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "math.hpp"
#include "string.hpp"
#include "ray.hpp"
#include "vec.hpp"
#include "clay.h"

constexpr i32 SCREEN_WIDTH  = 1600;
constexpr i32 SCREEN_HEIGHT = 900;

ray::FontId CLAY_FONT;

// Clay reports configuration and layout problems through this callback; LOG is
// the whole response — clay recovers internally and the frame carries on.
fn void clay_error(Clay_ErrorData error) { LOG("clay: %.*s", (int)error.errorText.length, error.errorText.chars); }

// clay measures per word-slice; zero FontId = ray's default-font fallback
// until Clay fontIds are mapped to loaded fonts. ray::measure_text uses the
// same font fallback and spacing as ray::draw_text, so measure and draw agree.
fn Clay_Dimensions clay_measure_text(Clay_StringSlice text, Clay_TextElementConfig* config, void*) {
    ray::TextMetrics metrics = ray::measure_text({(usize)text.length, text.chars}, CLAY_FONT, config->fontSize);
    return {metrics.size.x, metrics.size.y};
}

fn void clay_init(arena::Arena* arena) {
    // Clay lays out in one fixed block sized by its current settings (element
    // caps), carved from the eternal arena; it never allocates after init.
    // 64: clay aligns its internal allocations to 64-byte boundaries.
    usize      clay_memory_size = Clay_MinMemorySize();
    Clay_Arena clay_arena =
        Clay_CreateArenaWithCapacityAndMemory(clay_memory_size, arena::allocate_raw(arena, clay_memory_size, 64));
    Clay_Dimensions clay_dimensions = {(f32)SCREEN_WIDTH, (f32)SCREEN_HEIGHT};
    Clay_Initialize(clay_arena, clay_dimensions, {.errorHandlerFunction = clay_error});
    Clay_SetMeasureTextFunction(clay_measure_text, nullptr);
}

fn Clay_RenderCommandArray perform_ui() {
    auto mouse_pos    = ray::mouse_pos();
    b32  pointer_down = ray::mouse_down(ray::MouseButton::Left);
    Clay_SetPointerState({mouse_pos.x, mouse_pos.y}, pointer_down);
    Clay_BeginLayout();
    // Hello world: a full-screen root centering one rounded panel.
    CLAY({.id     = CLAY_ID("Root"),
          .layout = {.sizing         = {CLAY_SIZING_GROW(), CLAY_SIZING_GROW()},
                     .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}}}) {
        CLAY(Clay_ElementDeclaration{
            .id              = CLAY_ID("HelloPanel"),
            .layout          = {.sizing         = {CLAY_SIZING_FIXED(320), CLAY_SIZING_FIXED(180)},
                                .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
            .backgroundColor = {58, 63, 82, 255},
            .border          = {.color = {255, 0, 0, 255}, .width = CLAY_BORDER_ALL(4)},
            .cornerRadius    = CLAY_CORNER_RADIUS(8),
        }) {
            CLAY_TEXT(CLAY_STRING("Hello, Clay!"),
                      CLAY_TEXT_CONFIG({.textColor = {235, 238, 245, 255}, .fontSize = 24}));
        }
    }
    return Clay_EndLayout();
}

fn math::Rect from_clay(Clay_BoundingBox box) { return {box.x, box.y, box.width, box.height}; }

fn math::Color from_clay(Clay_Color color) { return {(u8)(color.r), (u8)(color.g), (u8)(color.b), (u8)(color.a)}; }

fn String from_clay(Clay_StringSlice str) { return {(usize)str.length, str.chars}; }

fn void render_clay_commands(Clay_RenderCommandArray commands) {
    for (auto idx = 0; idx < commands.length; ++idx) {
        const auto& cmd    = commands.internalArray[idx];
        auto        bounds = from_clay(cmd.boundingBox);
        switch (cmd.commandType) {
            case CLAY_RENDER_COMMAND_TYPE_NONE: break;
            case CLAY_RENDER_COMMAND_TYPE_RECTANGLE: {
                auto color  = from_clay(cmd.renderData.rectangle.backgroundColor);
                f32  radius = cmd.renderData.rectangle.cornerRadius.bottomLeft;
                ray::fill_rect(bounds, color, radius);
            } break;
            case CLAY_RENDER_COMMAND_TYPE_BORDER: {
                auto color  = from_clay(cmd.renderData.border.color);
                f32  radius = cmd.renderData.border.cornerRadius.bottomLeft;
                f32  thick  = cmd.renderData.border.width.bottom;
                ray::stroke_rect(bounds, thick, color, radius);
            } break;
            case CLAY_RENDER_COMMAND_TYPE_TEXT: {
                auto pos   = math::corner(bounds);
                pos.x      = (f32)(i64)pos.x;
                pos.y      = (f32)(i64)pos.y;
                auto color = from_clay(cmd.renderData.text.textColor);
                u16  size  = cmd.renderData.text.fontSize;
                auto text  = from_clay(cmd.renderData.text.stringContents);
                ray::draw_text(text, CLAY_FONT, pos, size, color);
            } break;
            default: break;
        }
    }
}

int main() {
    arena::Arena frame_arena;
    arena::reserve(&frame_arena, 256 * MB);

    arena::Arena eternal_arena;
    arena::reserve(&eternal_arena, 256 * MB);

    ray::window_open(SCREEN_WIDTH, SCREEN_HEIGHT, "Hello", true);

    clay_init(&eternal_arena);

    CLAY_FONT = ray::load_font_from_file("assets/fonts/default.ttf", 24).id;

    while (true) {
        if (ray::key_pressed(ray::Key::Escape)) { break; }
        auto ui_commands = perform_ui();

        ray::frame_begin();
        constexpr math::Color BKG_COLOR = math::rgb(28, 30, 38);
        ray::clear(BKG_COLOR);
        render_clay_commands(ui_commands);
        ray::frame_end();

        arena::reset(&frame_arena);
    }

    ray::window_close();
    return 0;
}
