#pragma once

#include "core.hpp"
#include "math.hpp"
#include "string.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "tabula.hpp"
#include "ui/data.hpp"
#include "vec.hpp"

namespace game {
using namespace arena;

// The map of the world. Use sensible defaults for memory allocation
constexpr usize MAP_MAX_WIDTH  = 2000;
constexpr usize MAP_MAX_HEIGHT = 2000;
constexpr usize MAP_MAX_SIZE   = MAP_MAX_WIDTH * MAP_MAX_HEIGHT;

// World-space pixels per cell edge; drawing and culling share it.
constexpr f32 CELL_SIZE = 24;

struct TerrainType {
    String      name;
    math::Color color;
    char        sigil;
    b32         passable;
};

inline const TerrainType TERRAIN_TYPES[] = {
    TerrainType{.name = "Water", .color = {32, 96, 168, 255}, .sigil = '~', .passable = false},
    TerrainType{.name = "Land", .color = {88, 140, 72, 255}, .sigil = '.', .passable = false},
    TerrainType{.name = "Road", .color = {148, 124, 88, 255}, .sigil = '#', .passable = true},
};

constexpr usize NUM_TERRAIN_TYPES = sizeof(TERRAIN_TYPES) / sizeof(TERRAIN_TYPES[0]);

struct WorldCell {
    u8 terrain_type_idx;
};

struct CellPos {
    i32 x;
    i32 y;
};

struct WorldMap {
    // Actual logical size of the map, in cells.
    i32 width;
    i32 height;
    // Grid of tiles
    Array<WorldCell, MAP_MAX_SIZE> grid;

    // Indexing outside the logical size is a bug: without the ASSERT an
    // x beyond width silently reads the next row.
    const WorldCell& operator[](CellPos pos) const {
        ASSERT(pos.x >= 0 && pos.x < width && pos.y >= 0 && pos.y < height);
        return grid[(usize)pos.x + (usize)width * (usize)pos.y];
    }

    WorldCell& operator[](CellPos pos) {
        ASSERT(pos.x >= 0 && pos.x < width && pos.y >= 0 && pos.y < height);
        return grid[(usize)pos.x + (usize)width * (usize)pos.y];
    }
};

struct Thing;

// Generational entity id;
struct Id {
    u16 slot;
    u16 generation;

    operator b32() const;
};

fn b32 is_valid(Id id) { return id.slot != 0 && id.generation % 2 == 1; }

fn Id::operator b32() const { return is_valid(*this); }

struct Name {
    usize offset;
    usize len;
};

struct Thing {
    Id id;
    // Slot of next entity, in the entity free-list, or the entity despawn list, etc
    u16 next;
    // Set by mark_for_despawn; the thing stays alive (handles resolve) until
    // despawn_flush. Guards against double-insertion into the despawn list.
    b32 marked_for_despawn;
    // Offset and length in name buffer
    Name name;
    // On-map position, only relevant for entities that are on map.
    CellPos cell_pos;
};

constexpr usize NUM_ENTITIES    = 32000;
constexpr usize NAME_BUFFER_LEN = 1024 * 1024;

// The probing counterpart of operator[]: callers that legitimately ask
// "is this cell on the map?" test here instead of trapping.
fn b32 in_bounds(const WorldMap* map, CellPos pos) {
    return pos.x >= 0 && pos.x < map->width && pos.y >= 0 && pos.y < map->height;
}

// Authoritative game state. ZII: the all-zero world is valid and empty —
// slot 0 is the permanently-zeroed dummy, the first spawn takes slot 1.
struct World {
    WorldMap world_map;
    // Dynamic array of char containing the name buffer
    DynArray<char, NAME_BUFFER_LEN> name_buffer;
    // Array of things
    Array<Thing, NUM_ENTITIES> things;
    // Head of the recycled-slot free list; 0 = none recycled yet
    u16 free_head;
    // Highest slot ever spawned; fresh slots come from bumping this
    u16 high_water;
    // Head of despawn list
    u16 despawn_head;
    // Global information
    // Days (in logical count)
    u64 epoch;
};

// Appends the name's bytes to the name buffer and returns their {offset, len}
// handle. Names are permanent: the buffer only grows, so handles stay valid
// for the whole run. A full buffer logs and returns the zero Name, which
// resolves to the empty string (ZII).
fn Name define_name(World* world, String name) {
    usize offset = world->name_buffer.len;
    if (!append(&world->name_buffer, name)) {
        LOG("name buffer full: define_name refused (%d bytes)", (int)NAME_BUFFER_LEN);
        return {};
    }
    return {offset, name.len};
}

fn String resolve_name(const World* world, Name name) {
    // Names only come from define_name; anything out of range is a bug.
    ASSERT(name.offset + name.len <= world->name_buffer.len);
    return {name.len, world->name_buffer.data + name.offset};
}

fn Thing* get(World* world, usize slot) { return &world->things[slot]; }

fn const Thing* get(const World* world, usize slot) { return get(const_cast<World*>(world), slot); }

fn Thing* get(World* world, Id id) {
    if (is_valid(id)) {
        auto* thing = &world->things[id.slot];
        if (id.generation == thing->id.generation) { return thing; }
    }
    return &world->things[0];
}

fn const Thing* get(const World* world, Id id) { return get(const_cast<World*>(world), id); }

fn Id spawn(World* world) {
    u16 slot = 0;
    if (world->free_head) {
        slot             = world->free_head;
        world->free_head = world->things[slot].next;
    } else if ((usize)world->high_water + 1 < world->things.len()) {
        slot = ++world->high_water;
    } else {
        LOG("world full: spawn refused (%d things)", (int)NUM_ENTITIES);
        return {};
    }

    Thing* thing = &world->things[slot];
    // Generation survives the reset: even (dead) -> odd (alive)
    u16 generation = thing->id.generation + 1;
    *thing         = {};
    thing->id      = {slot, generation};
    return thing->id;
}

// Park the thing on the despawn list. It stays fully alive — handles still
// resolve — until despawn_flush, so the world is consistent for the rest of
// the frame no matter when during it things get marked.
fn void mark_for_despawn(World* world, Id id) {
    Thing* thing = get(world, id);
    if (thing->id.slot == 0 || thing->marked_for_despawn) { return; }

    thing->marked_for_despawn = true;
    thing->next               = world->despawn_head;
    world->despawn_head       = thing->id.slot;
}

// Kill marked things and move their slots to the free list. Call once per
// frame, after all systems have run: this is the single point where handles
// die (generation odd -> even) and where slots become reusable.
fn void despawn_flush(World* world) {
    while (world->despawn_head) {
        u16    slot         = world->despawn_head;
        Thing* thing        = &world->things[slot];
        world->despawn_head = thing->next;

        thing->id.generation += 1;
        thing->marked_for_despawn = false;
        thing->next               = world->free_head;
        world->free_head          = slot;
    }
}

enum class CommandKind : u32 {
    Nil,
    Choose, // pick option choice_index in the open interaction
};

// Fat ZII payload: kind selects which fields mean anything, the rest stay
// zero. Revisit as a union-of-structs only if the fields start colliding.
struct Command {
    CommandKind kind;
    i32         choice_index;
};

// Read the sim's fields out of one action record ("choose = 2"). Absent or
// unusable fields leave the Nil command; the first present verb wins.
fn Command parse_command(const tabula::Node* root) {
    tabula::Value choose = tabula::get_value(root, "choose");
    if (choose.is_number) {
        Command result      = {};
        result.kind         = CommandKind::Choose;
        result.choice_index = (i32)choose.number;
        return result;
    }

    return {};
}

struct TickCommands {
    Slice<Command> commands;
    // Requested number of days that could have passed given
    // the current time flow
    usize num_days;
};

fn void tick(World* world, TickCommands commands) {
    // Process the commands...
    for (auto& command : commands.commands) {
        switch (command.kind) {
            case game::CommandKind::Nil: break;
            case game::CommandKind::Choose:
                // No interaction system yet; the log is the placeholder.
                LOG("game: choose %d", command.choice_index);
                break;
        }
    }

    // Process the number of days...
    for (usize day_idx = 0; day_idx < commands.num_days; day_idx++) {
        world->epoch++;
    }
}

fn ui::data::Data extract_ui_data(Arena* arena, const World* world) {
    auto data = ui::data::make(arena);

    {
        auto date = string::format(arena, "Year %d", world->epoch);
        ui::data::bind_global(&data, "DATE", date);
    }

    // {
    //     ui::data::bind_global(&data, "INTERACTION", "yes");
    //     ui::data::bind_global(&data, "INT_TITLE", "Title goes here");
    //     ui::data::bind_global(&data, "INT_TEXT", "The description would go here");
    //     ui::data::begin_list(&data, "choices");
    //     ui::data::begin_row(&data);
    //     ui::data::bind(&data, "INDEX", "1");
    //     ui::data::bind(&data, "CHOICE", "Yes");
    //     ui::data::begin_row(&data);
    //     ui::data::bind(&data, "INDEX", "2");
    //     ui::data::bind(&data, "CHOICE", "No");
    // }
    return data;
}

struct MapItem {
    math::Rect  bounds;
    math::Color color;
};

struct DrawMap {
    Slice<MapItem> items;
};

// Emits only the cells intersecting `visible` (the camera's view in world
// space), so the per-frame cost is bounded by screen size, not map size.
fn DrawMap draw_map(Arena* arena, const World* world, math::Rect visible) {
    const WorldMap* map = &world->world_map;

    // Cells are centered at (x, y) * CELL_SIZE. One cell of slack on each
    // edge covers the half-cell center offset and truncation-toward-zero on
    // negative coordinates; the clamp keeps operator[] in bounds.
    i32 x0 = clamp((i32)(visible.x / CELL_SIZE) - 1, 0, map->width);
    i32 x1 = clamp((i32)((visible.x + visible.w) / CELL_SIZE) + 2, 0, map->width);
    i32 y0 = clamp((i32)(visible.y / CELL_SIZE) - 1, 0, map->height);
    i32 y1 = clamp((i32)((visible.y + visible.h) / CELL_SIZE) + 2, 0, map->height);

    DrawMap out   = {};
    auto    items = vec::make_vec<MapItem>(arena, (usize)(x1 - x0) * (usize)(y1 - y0));
    auto    size  = math::splat(CELL_SIZE);
    for (i32 y = y0; y < y1; y++) {
        for (i32 x = x0; x < x1; x++) {
            auto  tile         = (*map)[CellPos{x, y}];
            auto& terrain_type = TERRAIN_TYPES[tile.terrain_type_idx];
            auto  bounds       = math::rect_with_center_and_size({x * size.x, y * size.y}, size);
            auto  item         = MapItem{
                         .bounds = bounds,
                         .color  = terrain_type.color,
            };
            vec::push(&items, item);
        }
    }
    out.items = vec::slice(items);
    return out;
}

namespace {
// Load the map from a text file: each character is one cell, matched against
// TerrainType::sigil. A row is a non-empty line — blank lines anywhere are
// formatting noise, and an intentional all-water row is written explicitly.
// The longest row sets width; ragged rows leave their tail cells zero
// (Water). The arena only holds the file bytes while parsing — a scratch
// watermark reclaims them on return. Any failure logs and leaves the map
// empty (ZII).
fn void load_world_map(Arena* arena, String path, WorldMap* world_map) {
    // Not `*world_map = {}`: that builds a multi-MB temporary on the stack.
    memset(world_map, 0, sizeof(*world_map));

    ScratchArena scratch(arena);

    file_io::ReadFile<String> source = file_io::read_file_to_string(arena, path);
    if (source.messages.len) {
        String message = (*source.messages.begin()).message;
        LOG("map: %.*s — map stays empty", (int)message.len, message.data);
        return;
    }

    // Pass 1: dimensions. Only non-empty lines count as rows.
    usize width    = 0;
    usize height   = 0;
    usize line_len = 0;
    for (char c : source.data) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (line_len) {
                width = max(width, line_len);
                height += 1;
            }
            line_len = 0;
            continue;
        }
        line_len += 1;
    }
    if (line_len) {
        width = max(width, line_len);
        height += 1;
    }

    if (width > MAP_MAX_WIDTH || height > MAP_MAX_HEIGHT) {
        LOG("map: %.*s is %dx%d, max is %dx%d — clipping", (int)path.len, path.data, (int)width, (int)height,
            (int)MAP_MAX_WIDTH, (int)MAP_MAX_HEIGHT);
        width  = min(width, MAP_MAX_WIDTH);
        height = min(height, MAP_MAX_HEIGHT);
    }
    world_map->width  = (i32)width;
    world_map->height = (i32)height;

    // Pass 2: fill cells. x doubles as the has-content test on newline, so
    // empty lines advance no row here either.
    b32 logged_unknown = false;
    i32 x              = 0;
    i32 y              = 0;
    for (char c : source.data) {
        if (c == '\r') continue;
        if (c == '\n') {
            if (x > 0) y += 1;
            x = 0;
            continue;
        }
        if ((usize)x < width && (usize)y < height) {
            u8  idx   = 0;
            b32 found = false;
            for (usize t = 0; t < NUM_TERRAIN_TYPES; t++) {
                if (TERRAIN_TYPES[t].sigil == c) {
                    idx   = (u8)t;
                    found = true;
                    break;
                }
            }
            if (!found && !logged_unknown) {
                LOG("map: unknown sigil '%c' at %d,%d — cells left as %.*s", c, x, y, (int)TERRAIN_TYPES[0].name.len,
                    TERRAIN_TYPES[0].name.data);
                logged_unknown = true;
            }
            (*world_map)[{x, y}].terrain_type_idx = idx;
        }
        x += 1;
    }
}
} // namespace

fn void initialize(Arena* arena, World* world) { load_world_map(arena, "data/map.txt", &world->world_map); }

} // namespace game
