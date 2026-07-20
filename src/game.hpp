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

// Double braces: the outer pair initializes the Array struct, the inner its
// embedded data[] — single braces make element one claim the whole member.
inline const Array<TerrainType, 3> TERRAIN_TYPES = {{
    {.name = "Water", .color = {32, 96, 168, 255}, .sigil = '~', .passable = false},
    {.name = "Land", .color = {88, 140, 72, 255}, .sigil = '.', .passable = false},
    {.name = "Road", .color = {148, 124, 88, 255}, .sigil = '#', .passable = true},
}};

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

enum class BodyShape {
    Rectangle,
    Circle,
};

struct BodyKind {
    // Id name to be resolved
    String name;
    // Size, in cells.
    f32 size;
    // Shape
    BodyShape style;
};

// Double braces: the outer pair initializes the Array struct, the inner its
// embedded data[] — single braces make element one claim the whole member.
inline const Array<BodyKind, 3> BODY_KINDS = {{
    {},
    {.name = "person", .size = 0.5f, .style = BodyShape::Circle},
    {.name = "town", .size = 1.0f, .style = BodyShape::Rectangle},
}};

fn u8 lookup_body_kind(String name) {
    for (u8 idx = 0; idx < BODY_KINDS.len(); ++idx) {
        if (BODY_KINDS[idx].name == name) { return idx; }
    }
    return 0;
}

enum class Hierarchy : u8 {
    LocationOf,
    Count,
};

constexpr usize HIERARCHY_COUNT = (usize)Hierarchy::Count;

struct Thing {
    Id id;
    // Set by mark_for_despawn; the thing stays alive (handles resolve) until
    // despawn_flush. Guards against double-insertion into the despawn list.
    b32 marked_for_despawn;
    // Offset and length in name buffer
    Name name;
    // Is the entity something that exists as a phyisical entity in the world?
    u8 body_kind_idx;
    // On-map position, only relevant for entities that are on map.
    CellPos cell_pos;
    // Hierarchy parents
    Array<Id, HIERARCHY_COUNT> parents;
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
    // Slots ready for reuse; spawn pops from the end. Sized for every slot,
    // so pushes cannot fail.
    DynArray<u16, NUM_ENTITIES> free_slots;
    // Slots parked by mark_for_despawn, drained by despawn_flush. At most one
    // entry per live thing (marked_for_despawn dedupes), so pushes cannot fail.
    DynArray<u16, NUM_ENTITIES> despawn_list;
    // Highest slot ever spawned; fresh slots come from bumping this
    u16 high_water;
    // Bumped by set_parent (its hierarchy) and despawn_flush (all): derived
    // indexes (Game) compare against these to see they are stale. Compared
    // with !=, never <, so u32 wrap-around is harmless.
    Array<u32, HIERARCHY_COUNT> parents_version;
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

fn Thing* spawn(World* world) {
    u16 slot = 0;
    if (world->free_slots.len) {
        slot = world->free_slots[world->free_slots.len - 1];
        pop(&world->free_slots);
    } else if ((usize)world->high_water + 1 < world->things.len()) {
        slot = ++world->high_water;
    } else {
        LOG("world full: spawn refused (%d things)", (int)NUM_ENTITIES);
        return &world->things[0];
    }

    auto* thing = &world->things[slot];
    // Generation survives the reset: even (dead) -> odd (alive)
    u16 generation = thing->id.generation + 1;
    *thing         = {};
    thing->id      = {slot, generation};
    return thing;
}

// Park the thing on the despawn list. It stays fully alive — handles still
// resolve — until despawn_flush, so the world is consistent for the rest of
// the frame no matter when during it things get marked.
fn void mark_for_despawn(World* world, Id id) {
    Thing* thing = get(world, id);
    if (thing->id.slot == 0 || thing->marked_for_despawn) { return; }

    thing->marked_for_despawn = true;
    b32 pushed                = push(&world->despawn_list, thing->id.slot);
    ASSERT(pushed);
}

// Kill marked things and move their slots to the free list. Call once per
// frame, after all systems have run: this is the single point where handles
// die (generation odd -> even) and where slots become reusable.
fn void despawn_flush(World* world) {
    if (world->despawn_list.len) {
        // Freed slots may be reused: every derived index must revalidate, so
        // no CSR row can outlive the occupant it was built for.
        for (usize h = 0; h < HIERARCHY_COUNT; h++) {
            world->parents_version[h] += 1;
        }
    }
    for (u16 slot : world->despawn_list) {
        Thing* thing = &world->things[slot];
        thing->id.generation += 1;
        thing->marked_for_despawn = false;
        b32 pushed                = push(&world->free_slots, slot);
        ASSERT(pushed);
    }
    clear(&world->despawn_list);
}

// The one blessed write to a thing's hierarchy parent: bumps the hierarchy's
// version so derived indexes know to rebuild. Bump-on-change only; a stale
// child handle resolves to the dummy and no-ops. No cycle checks — the
// caller's problem until a real need appears.
fn void set_parent(World* world, Id child, Hierarchy hierarchy, Id parent) {
    Thing* thing = get(world, child);
    if (thing->id.slot == 0) { return; }

    usize h       = (usize)hierarchy;
    Id    current = thing->parents[h];
    if (current.slot == parent.slot && current.generation == parent.generation) { return; }
    thing->parents[h] = parent;
    world->parents_version[h] += 1;
}

// row_start entries are prefix sums of edge counts; one parent per thing
// caps them at NUM_ENTITIES - 1, so u16 holds them exactly as long as this
// holds (and it always does while slots are u16).
static_assert(NUM_ENTITIES <= 65536, "hierarchy row_start is u16");

// One hierarchy's children lists in CSR form, rebuilt from Thing::parents.
struct HierarchyIndex {
    // Mirrors World::parents_version at last rebuild; != means stale.
    u32 built_version;
    // Children of slot s live at children[row_start[s] .. row_start[s+1]).
    Array<u16, NUM_ENTITIES + 1> row_start;
    Array<u16, NUM_ENTITIES>     children;
};

// The sim root: the authoritative World plus derived, rebuildable structures.
// ZII: the zero Game is consistent — never-built indexes (version 0) match a
// World whose parents were never touched, and both read as "no children".
struct Game {
    World                                  world;
    Array<HierarchyIndex, HIERARCHY_COUNT> hierarchies;
    // Fill-pass cursor scratch for hierarchy_refresh; meaningless between calls.
    Array<u16, NUM_ENTITIES + 1> cursor;
};

// Rebuild every stale hierarchy index: a counting sort over live things,
// dropping edges whose child is dead or whose parent handle no longer
// resolves. Runs where mutations stop — end of initialize and end of tick.
fn void hierarchy_refresh(Game* game) {
    World* world = &game->world;
    for (usize h = 0; h < HIERARCHY_COUNT; h++) {
        HierarchyIndex* index = &game->hierarchies[h];
        if (index->built_version == world->parents_version[h]) { continue; }

        memset(&index->row_start, 0, sizeof(index->row_start));
        for (usize slot = 1; slot <= world->high_water; slot++) {
            const Thing& thing = world->things[slot];
            if (!is_valid(thing.id)) { continue; }
            const Thing* parent = get(world, thing.parents[h]);
            if (parent->id.slot == 0) { continue; }
            index->row_start[(usize)parent->id.slot + 1] += 1;
        }
        for (usize i = 1; i <= NUM_ENTITIES; i++) {
            index->row_start[i] += index->row_start[i - 1];
        }

        game->cursor = index->row_start;
        for (usize slot = 1; slot <= world->high_water; slot++) {
            const Thing& thing = world->things[slot];
            if (!is_valid(thing.id)) { continue; }
            const Thing* parent = get(world, thing.parents[h]);
            if (parent->id.slot == 0) { continue; }
            index->children[game->cursor[parent->id.slot]++] = (u16)slot;
        }

        index->built_version = world->parents_version[h];
    }
}

// View over one CSR row; range-for yields ThingT* resolved against the
// world's things array on the fly. Valid until the next hierarchy_refresh.
template <typename ThingT> struct ChildrenView {
    const u16* slots;
    usize      len;
    ThingT*    things; // base of World::things

    struct Iter {
        const u16* at;
        ThingT*    things;

        ThingT* operator*() const { return &things[*at]; }
        Iter&   operator++() {
            at += 1;
            return *this;
        }
        bool operator!=(Iter other) const { return at != other.at; }
    };

    Iter begin() const { return {slots, things}; }
    Iter end() const { return {slots + len, things}; }
};

// Children of `parent` in one hierarchy. Freshness is a contract, not a
// fallback: a version mismatch is an ordering bug (a query before the
// refresh at end of initialize/tick), so it traps. A stale or nil parent
// resolves to the dummy, whose row is structurally empty. Slot reuse cannot
// serve another thing's row: reuse implies a despawn_flush, which bumped the
// versions and forced a rebuild before this ASSERT lets a read through.
fn ChildrenView<Thing> children_of(Game* game, Hierarchy hierarchy, Id parent) {
    usize           h     = (usize)hierarchy;
    HierarchyIndex* index = &game->hierarchies[h];
    ASSERT(index->built_version == game->world.parents_version[h]);

    u16 slot  = get(&game->world, parent)->id.slot;
    u32 start = index->row_start[slot];
    u32 end   = index->row_start[(usize)slot + 1];
    return {index->children.data + start, end - start, game->world.things.data};
}

fn ChildrenView<const Thing> children_of(const Game* game, Hierarchy hierarchy, Id parent) {
    ChildrenView<Thing> view = children_of(const_cast<Game*>(game), hierarchy, parent);
    return {view.slots, view.len, view.things};
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

fn void tick(Game* game, TickCommands commands) {
    World* world = &game->world;

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

    // By default, we process n ticks = to the number of days, and are all considered "advances"
    usize num_ticks   = commands.num_days;
    b32   advance_day = num_ticks > 0;
    num_ticks         = max<usize>(num_ticks, 1);

    for (usize tick_idx = 0; tick_idx < num_ticks; tick_idx++) {
        if (advance_day) { world->epoch++; }

        // Mutations stop here: derived indexes catch up before anyone reads them.
        hierarchy_refresh(game);
    }
}

fn ui::data::Data extract_ui_data(Arena* arena, const Game* game) {
    const World* world = &game->world;
    auto         data  = ui::data::make(arena);

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
    BodyShape   style;
    math::Rect  bounds;
    math::Color color;
};

struct DrawMap {
    Slice<MapItem> items;
};

// Emits only the cells intersecting `visible` (the camera's view in world
// space), so the per-frame cost is bounded by screen size, not map size.
fn DrawMap draw_map(Arena* arena, const Game* game, math::Rect visible) {
    const World*    world = &game->world;
    const WorldMap* map   = &world->world_map;

    // Cells are centered at (x, y) * CELL_SIZE. One cell of slack on each
    // edge covers the half-cell center offset and truncation-toward-zero on
    // negative coordinates; the clamp keeps operator[] in bounds.
    i32 x0 = clamp((i32)(visible.x / CELL_SIZE) - 1, 0, map->width);
    i32 x1 = clamp((i32)((visible.x + visible.w) / CELL_SIZE) + 2, 0, map->width);
    i32 y0 = clamp((i32)(visible.y / CELL_SIZE) - 1, 0, map->height);
    i32 y1 = clamp((i32)((visible.y + visible.h) / CELL_SIZE) + 2, 0, map->height);

    // One per visible rectangle
    usize area_size = (usize)(x1 - x0) * (usize)(y1 - y0);
    // Extra room for things inside the terrian rectangle
    usize approx_num_items = area_size * 2;

    DrawMap out       = {};
    auto    items     = vec::make_vec<MapItem>(arena, approx_num_items);
    auto    cell_size = math::splat(CELL_SIZE);
    for (i32 y = y0; y < y1; y++) {
        for (i32 x = x0; x < x1; x++) {
            auto  tile         = (*map)[CellPos{x, y}];
            auto& terrain_type = TERRAIN_TYPES[tile.terrain_type_idx];
            auto  bounds       = math::rect_with_center_and_size({x * cell_size.x, y * cell_size.y}, cell_size);
            auto  item         = MapItem{
                         .style  = BodyShape::Rectangle,
                         .bounds = bounds,
                         .color  = terrain_type.color,
            };
            vec::push(&items, item);
        }
    }

    // (For now done here, later cache?)
    // Traverse all entities and draw the visible ones
    for (usize slot = 1; slot <= world->high_water; slot++) {
        const Thing& thing = world->things[slot];
        // Dead slots keep their old fields until reuse — parity filters them.
        if (!is_valid(thing.id)) { continue; }
        const auto* body_kind = &BODY_KINDS[thing.body_kind_idx];
        if (body_kind->size <= 0.0) { continue; }

        math::V2 size = math::splat(CELL_SIZE * body_kind->size);

        math::V2 pos    = {thing.cell_pos.x * cell_size.x, thing.cell_pos.y * cell_size.y};
        auto     bounds = math::rect_with_center_and_size(pos, size);
        // Cull by overlap, not center: a half-visible body still draws.
        auto clipped = math::intersect(visible, bounds);
        if (clipped.w <= 0 || clipped.h <= 0) { continue; }
        MapItem item = {
            .style  = body_kind->style,
            .bounds = bounds,
            .color  = {255, 255, 0, 255},
        };
        vec::push(&items, item);
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
            for (usize t = 0; t < TERRAIN_TYPES.len(); t++) {
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

fn void initialize(Arena* arena, Game* game) {
    World* world = &game->world;
    load_world_map(arena, "data/map.txt", &world->world_map);

    // The setup tree is only read here: a scratch watermark reclaims the file
    // bytes and nodes on return. Names survive by copy into the name buffer.
    ScratchArena scratch(arena);

    file_io::ReadFile<String> source = file_io::read_file_to_string(arena, "data/setup.txt");
    if (source.messages.len) {
        String message = (*source.messages.begin()).message;
        LOG("setup: %.*s — nothing spawned", (int)message.len, message.data);
        return;
    }

    tabula::ParseResult result = tabula::parse(arena, source.data);
    for (auto& error : result.errors) {
        LOG("setup: %.*s", (int)error.message.len, error.message.data);
    }

    for (const auto& root : result.roots) {
        if (root.key == "entity") {
            auto* thing = spawn(world);
            if (!thing->id) { break; }

            thing->name = define_name(world, tabula::get_text(&root, "name"));

            auto kind            = tabula::get_text(&root, "kind");
            thing->body_kind_idx = lookup_body_kind(kind);

            if (!thing->body_kind_idx) { LOG("setup: unknown body kind '%.*s'", (int)kind.len, kind.data); }

            auto pos          = tabula::get(&root, "pos");
            thing->cell_pos.x = (i32)tabula::item_number(pos, 0);
            thing->cell_pos.y = (i32)tabula::item_number(pos, 1);
        }
    }

    // Mutations stop here, same contract as tick: indexes fresh before the
    // first frame reads.
    hierarchy_refresh(game);
}

} // namespace game
