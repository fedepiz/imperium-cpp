#pragma once

#include "core.hpp"
#include "math.hpp"
#include "string.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "hashtable.hpp"
#include "tabula.hpp"
#include "ui/data.hpp"
#include "vec.hpp"

namespace game {
using namespace arena;

// Map size caps; WorldMap embeds tile storage for the full cap.
constexpr usize MAP_MAX_WIDTH  = 500;
constexpr usize MAP_MAX_HEIGHT = 500;
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

    bool operator==(const CellPos&) const = default;
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
    bool operator==(const Id&) const = default;
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
    // Cells per day; 0 = immobile (order_move refuses). Max 1: a thing never
    // moves more than one cell per day-tick, which is what lets movement
    // split into a gather pass and a sequential resolve pass (validate LOGs
    // anything above).
    f32 speed;
};

// Double braces: the outer pair initializes the Array struct, the inner its
// embedded data[] — single braces make element one claim the whole member.
inline const Array<BodyKind, 3> BODY_KINDS = {{
    {},
    {.name = "person", .size = 0.5f, .style = BodyShape::Circle, .speed = 1.0f},
    {.name = "town", .size = 1.0f, .style = BodyShape::Rectangle, .speed = 0},
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
    // Move order: the destination THING (zero = no order, ZII). The goal cell
    // re-resolves through the dest's LocationOf chain each day — traveling to
    // someone inside a town is traveling to the town. Canceled (LOG) when the
    // dest despawns or no road reaches it.
    Id move_dest;
    // Fractional step budget, in cells. Gains BodyKind::speed per day, capped
    // at 1 (max speed is one cell per day); a step costs 1; zeroed on
    // arrival/cancel.
    f32 move_points;
};

constexpr usize NUM_ENTITIES    = 32000;
constexpr usize NAME_BUFFER_LEN = 1024 * 1024;

// The probing counterpart of operator[]: callers that legitimately ask
// "is this cell on the map?" test here instead of trapping.
fn b32 in_bounds(const WorldMap* map, CellPos pos) {
    return pos.x >= 0 && pos.x < map->width && pos.y >= 0 && pos.y < map->height;
}

// Row-major flat index of a cell. Cell indices are usize/u32 territory — the
// map cap is 250k cells, past u16 — only coordinates pack into u16.
fn usize cell_index(const WorldMap* map, CellPos pos) {
    ASSERT(in_bounds(map, pos));
    return (usize)pos.x + (usize)map->width * (usize)pos.y;
}

// Interaction state
struct Choice {
    DynString<256> text;
    b32            enabled;
};

struct Interaction {
    // False = no interaction
    b32                   active;
    DynString<256>        title;
    DynString<2048>       description;
    DynArray<Choice, 256> choices;
};

fn void add_choice(Interaction* interaction, String text, b32 enabled) {
    if (push(&interaction->choices, {})) {
        Choice* choice  = &interaction->choices[interaction->choices.len - 1];
        choice->text    = text;
        choice->enabled = enabled;
    }
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
    // Current interaction
    Interaction interaction;
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

// A thing's parent handle in one hierarchy — the raw handle, possibly stale.
fn Id get_parent(const Thing* thing, Hierarchy hierarchy) { return thing->parents[(usize)hierarchy]; }

// Resolving overloads: the dummy when the parent is nil, dead, or stale.
fn Thing* get_parent(World* world, const Thing* thing, Hierarchy hierarchy) {
    return get(world, get_parent(thing, hierarchy));
}

fn const Thing* get_parent(const World* world, const Thing* thing, Hierarchy hierarchy) {
    return get(world, get_parent(thing, hierarchy));
}

// Linear name scan — names are few and this runs at setup/debug cadence.
// Zero id when absent or the name is empty.
fn Id find_thing_by_name(const World* world, String name) {
    if (!name.len) return {};
    for (usize slot = 1; slot <= world->high_water; slot++) {
        const Thing* thing = &world->things[slot];
        if (!thing->id) continue;
        if (resolve_name(world, thing->name) == name) return thing->id;
    }
    return {};
}

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
    if (!thing->id || thing->marked_for_despawn) { return; }

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
    if (!thing->id) { return; }

    usize h = (usize)hierarchy;
    if (get_parent(thing, hierarchy) == parent) { return; }
    thing->parents[h] = parent;
    world->parents_version[h] += 1;
}

// Walk a thing's chain in one hierarchy to its root — the outermost ancestor
// whose own parent no longer resolves (nil, dead, and stale all read as "no
// parent"). The thing itself when parentless; dummy for a dead id. Bounded
// against parent cycles (set_parent does no cycle checks): overflow LOGs and
// returns the dummy.
fn Thing* find_root_parent(World* world, Id id, Hierarchy hierarchy) {
    Thing* thing = get(world, id);
    for (u32 hops = 0; hops < 64 && thing->id; hops++) {
        Thing* parent = get_parent(world, thing, hierarchy);
        if (!parent->id) return thing;
        thing = parent;
    }
    if (thing->id) { LOG("hierarchy: parent cycle at slot %d", (int)thing->id.slot); }
    return &world->things[0];
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

enum class EventKind : u32 {
    Nil,     // zero — ZII default
    Meet,    // a stepped into b's cell
    Arrival, // a completed its move order at b (its dest)
};

// Fat ZII payload: kind selects the field roles —
// a is always the mover whose step emitted the event; b is the other party
// (Meet) or the ordered destination (Arrival). Meeting something and
// finishing an order are independent facts: one step may emit both, and the
// stream keeps them in emission order.
struct Event {
    EventKind kind;
    Id        a;
    Id        b;
    CellPos   cell;
};

constexpr usize MAX_EVENTS      = 1024;
constexpr usize GAME_ARENA_SIZE = 64 * MB; // vmem reserve; committed stays ~9MB at the map cap

// The spatial subsystem: the live "who is where" grid plus the pathfinding
// state that serves it. The grid is a derived-but-synced cache over
// Thing::cell_pos, exact at every instant: every position/on-mapness
// mutation goes through the blessed mutators (spatial_link/unlink,
// move_thing, exit_to_map, enter_container, the Game-level despawn_flush).
// No allocation in any op except spatial_initialize; validate() audits the
// sync.
struct SpatialMap {
    // Per-cell chain head (slot id), world_map.width*height entries, 0 =
    // empty (slot 0 is the dummy, never linked). Null until
    // spatial_initialize — ZII: reads see an empty grid.
    u16* cell_heads;
    // Intrusive chain through co-located slots; 0 terminates. cell_next[0]
    // stays 0 forever — it doubles as the end sentinel.
    Array<u16, NUM_ENTITIES> cell_next;

    // BFS scratch, width*height entries each. came_from[i] is predecessor
    // cell index + 1; 0 = unvisited. Meaningless between searches.
    u32* bfs_came_from;
    u32* bfs_queue;

    // pack_route_key(from, dest) -> packed next cell. Sized so a freshly
    // purged table absorbs any whole-path insert batch without growing —
    // growth from a whole-run arena would leak the old block (see
    // route_next_hop's purge).
    hashtable::Hashtable<u32> next_hop_cache;
};

// The sim root: the authoritative World plus derived, rebuildable structures.
// ZII: the zero Game is consistent — never-built indexes (version 0) match a
// World whose parents were never touched, and both read as "no children".
// Game must never relocate: spatial.next_hop_cache stores &this->arena.
struct Game {
    World                                  world;
    Array<HierarchyIndex, HIERARCHY_COUNT> hierarchies;
    // Fill-pass cursor scratch for hierarchy_refresh; meaningless between calls.
    Array<u16, NUM_ENTITIES + 1> cursor;

    // Whole-run arena owned by the game: spatial grid, BFS scratch, path
    // cache, plus tick-time ScratchArena watermarks. Reserved at startup by
    // whoever owns the Game (initialize, tests) before spatial_initialize
    // allocates from it.
    Arena arena;

    SpatialMap spatial;

    // This day's movement events: cleared at day start, LOG-drained at day
    // end, left in place so tests can inspect them between ticks.
    DynArray<Event, MAX_EVENTS> events;
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
            if (!thing.id) { continue; }
            const Thing* parent = get_parent(world, &thing, (Hierarchy)h);
            if (!parent->id) { continue; }
            index->row_start[(usize)parent->id.slot + 1] += 1;
        }
        for (usize i = 1; i <= NUM_ENTITIES; i++) {
            index->row_start[i] += index->row_start[i - 1];
        }

        game->cursor = index->row_start;
        for (usize slot = 1; slot <= world->high_water; slot++) {
            const Thing& thing = world->things[slot];
            if (!thing.id) { continue; }
            const Thing* parent = get_parent(world, &thing, (Hierarchy)h);
            if (!parent->id) { continue; }
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

// --- Spatial grid ------------------------------------------------------------

// On-map: has a body AND the LocationOf parent resolves to the dummy (nil and
// stale handles both count as "no container"). Dead slots are the caller's
// filter — the dummy itself reads as off-map via its zero body.
fn b32 is_on_map(const World* world, const Thing* thing) {
    if (thing->body_kind_idx == 0) return false;
    return !get_parent(world, thing, Hierarchy::LocationOf)->id;
}

// The cell owner for a thing: the root of its LocationOf chain, IF that root
// actually stands on the map (the thing itself when already on map). A chain
// can root in something bodiless — there is no cell to inherit — and that
// reads as "nowhere": the dummy.
fn Thing* resolve_map_anchor(World* world, Id id) {
    Thing* root = find_root_parent(world, id, Hierarchy::LocationOf);
    return is_on_map(world, root) ? root : &world->things[0];
}

// Head-insert into the thing's cell chain. Caller guarantees: alive, on-map,
// in-bounds, not already linked — a double link corrupts silently; validate()
// is the audit that catches maintenance bugs.
fn void spatial_link(SpatialMap* spatial, const WorldMap* map, Thing* thing) {
    ASSERT(spatial->cell_heads);
    u16 slot = thing->id.slot;
    ASSERT(slot != 0);
    usize cell                = cell_index(map, thing->cell_pos);
    spatial->cell_next[slot]  = spatial->cell_heads[cell];
    spatial->cell_heads[cell] = slot;
}

// Walk-to-unlink — chains are short (co-located things), so no prev[] until
// profiling says otherwise. ASSERTs the thing is in its cell's chain: an
// absent entry means a maintenance bug, not a recoverable state.
fn void spatial_unlink(SpatialMap* spatial, const WorldMap* map, Thing* thing) {
    ASSERT(spatial->cell_heads);
    u16 slot = thing->id.slot;
    ASSERT(slot != 0);
    usize cell = cell_index(map, thing->cell_pos);
    if (spatial->cell_heads[cell] == slot) {
        spatial->cell_heads[cell] = spatial->cell_next[slot];
    } else {
        u16 at = spatial->cell_heads[cell];
        while (at && spatial->cell_next[at] != slot) {
            at = spatial->cell_next[at];
        }
        ASSERT(at);
        spatial->cell_next[at] = spatial->cell_next[slot];
    }
    spatial->cell_next[slot] = 0;
}

// Full recompute: clear, then spatial_link every live on-map thing — the
// incremental op IS the rebuild body, so both paths share one codepath.
// Out-of-bounds cell_pos is a data error: LOG and leave unlinked (validate
// reports it too). End of initialize only; ticks maintain incrementally.
fn void spatial_rebuild(SpatialMap* spatial, World* world) {
    if (!spatial->cell_heads) return; // ZII: no grid to build
    const WorldMap* map = &world->world_map;
    memset(spatial->cell_heads, 0, (usize)map->width * (usize)map->height * sizeof(u16));
    memset(&spatial->cell_next, 0, sizeof(spatial->cell_next));
    for (usize slot = 1; slot <= world->high_water; slot++) {
        Thing* thing = &world->things[slot];
        if (!thing->id || !is_on_map(world, thing)) continue;
        if (!in_bounds(map, thing->cell_pos)) {
            LOG("spatial: slot %d off the map at %d,%d — not linked", (int)slot, thing->cell_pos.x, thing->cell_pos.y);
            continue;
        }
        spatial_link(spatial, map, thing);
    }
}

// View over one cell's occupants; range-for yields ThingT* resolved against
// the world's things array on the fly (same shim shape as ChildrenView).
// Valid only while the chains aren't mutated — don't step movers mid-iteration.
template <typename ThingT> struct CellView {
    const u16* next;   // base of SpatialMap::cell_next
    ThingT*    things; // base of World::things
    u16        head;

    struct Iter {
        const u16* next;
        ThingT*    things;
        u16        at;

        ThingT* operator*() const { return &things[at]; }
        Iter&   operator++() {
            at = next[at];
            return *this;
        }
        bool operator!=(Iter other) const { return at != other.at; }
    };

    Iter begin() const { return {next, things, head}; }
    Iter end() const { return {next, things, 0}; }
};

fn CellView<Thing> occupants_of(SpatialMap* spatial, World* world, CellPos cell) {
    u16 head = 0;
    if (spatial->cell_heads && in_bounds(&world->world_map, cell)) {
        head = spatial->cell_heads[cell_index(&world->world_map, cell)];
    }
    return {spatial->cell_next.data, world->things.data, head};
}

fn CellView<const Thing> occupants_of(const SpatialMap* spatial, const World* world, CellPos cell) {
    CellView<Thing> view = occupants_of(const_cast<SpatialMap*>(spatial), const_cast<World*>(world), cell);
    return {view.next, view.things, view.head};
}

// THE blessed cell_pos write for a linked (on-map) thing.
fn void move_thing(SpatialMap* spatial, const WorldMap* map, Thing* thing, CellPos cell) {
    spatial_unlink(spatial, map, thing);
    thing->cell_pos = cell;
    spatial_link(spatial, map, thing);
}

// Surface a contained thing on the map at its outermost on-map ancestor's
// cell: write cell_pos, clear the LocationOf parent (through set_parent, so
// the hierarchy version bumps), link if bodied. False + LOG when the chain
// never reaches the map. No-op (true) when already on map.
fn b32 exit_to_map(SpatialMap* spatial, World* world, Thing* thing) {
    if (is_on_map(world, thing)) return true;
    Thing* anchor = resolve_map_anchor(world, thing->id);
    if (!anchor->id) {
        LOG("spatial: slot %d has no map anchor to exit to", (int)thing->id.slot);
        return false;
    }
    thing->cell_pos = anchor->cell_pos;
    set_parent(world, thing->id, Hierarchy::LocationOf, {});
    if (thing->body_kind_idx != 0) { spatial_link(spatial, &world->world_map, thing); }
    return true;
}

// Blessed containment: unlink from the map when on it, then set_parent.
// The container must resolve — leaving the map without a container is
// exit_to_map's job, not a nil-parent write through this path.
fn void enter_container(SpatialMap* spatial, World* world, Id child, Id container) {
    // A dead child rides ZII: is_on_map is false and set_parent no-ops.
    Thing* thing = get(world, child);
    ASSERT(get(world, container)->id);
    if (is_on_map(world, thing)) { spatial_unlink(spatial, &world->world_map, thing); }
    set_parent(world, child, Hierarchy::LocationOf, container);
}

// Grid-aware flush, the overload to call once a world has a spatial map.
// Order matters:
//   1. unlink the dying while their fields are still readable,
//   2. spill: survivors whose LocationOf parent is dying surface at the
//      container's last (frozen) cell — linear scan, because children_of may
//      be stale mid-tick and would trap on its freshness ASSERT,
//   3. the World-level flush recycles the slots.
// Marked containers inside marked containers die unspilled; live containers
// inside dying ones surface with their own occupants intact.
fn void despawn_flush(SpatialMap* spatial, World* world) {
    const WorldMap* map = &world->world_map;
    for (u16 slot : world->despawn_list) {
        Thing* thing = &world->things[slot];
        if (is_on_map(world, thing) && spatial->cell_heads && in_bounds(map, thing->cell_pos)) {
            spatial_unlink(spatial, map, thing);
        }
    }
    if (world->despawn_list.len) {
        for (usize slot = 1; slot <= world->high_water; slot++) {
            Thing* thing = &world->things[slot];
            if (!thing->id || thing->marked_for_despawn) continue;
            Thing* parent = get_parent(world, thing, Hierarchy::LocationOf);
            if (!parent->id || !parent->marked_for_despawn) continue;
            thing->cell_pos = parent->cell_pos;
            set_parent(world, thing->id, Hierarchy::LocationOf, {});
            if (thing->body_kind_idx != 0 && spatial->cell_heads && in_bounds(map, thing->cell_pos)) {
                spatial_link(spatial, map, thing);
            }
        }
    }
    despawn_flush(world);
}

// Allocate everything sized to the map's ACTUAL area — grid heads, BFS
// scratch, next-hop cache — from the given arena (already reserved by the
// caller; the spatial map allocates but never reserves). The only allocating
// spatial function. A zero-area map leaves the grid null (ZII); allocation
// failure LOGs and leaves it null too.
fn void spatial_initialize(SpatialMap* spatial, const WorldMap* map, Arena* arena) {
    usize area = (usize)map->width * (usize)map->height;
    if (!area) {
        LOG("spatial: empty map — grid stays null");
        return;
    }
    spatial->cell_heads    = (u16*)arena::allocate_raw(arena, area * sizeof(u16), alignof(u16));
    spatial->bfs_came_from = (u32*)arena::allocate_raw(arena, area * sizeof(u32), alignof(u32));
    spatial->bfs_queue     = (u32*)arena::allocate_raw(arena, area * sizeof(u32), alignof(u32));
    // Capacity such that a freshly purged table absorbs any whole-path batch
    // (max path < area cells) below the grow threshold — the cache provably
    // never grows, so nothing leaks into the whole-run arena.
    spatial->next_hop_cache = hashtable::make_table<u32>(arena, (area * 4) / 3 + 8);
    if (!spatial->cell_heads || !spatial->bfs_came_from || !spatial->bfs_queue) {
        LOG("spatial: arena exhausted — grid stays null");
        spatial->cell_heads = 0;
    }
}

// --- Pathfinding -------------------------------------------------------------

fn u32 pack_cell(CellPos pos) { return ((u32)(u16)pos.x << 16) | (u32)(u16)pos.y; }

fn CellPos unpack_cell(u32 packed) { return {(i32)(packed >> 16), (i32)(packed & 0xFFFF)}; }

fn CellPos cell_from_index(const WorldMap* map, u32 index) {
    return {(i32)(index % (u32)map->width), (i32)(index / (u32)map->width)};
}

// 4 x u16 coordinates in a u64. Zero would need from == dest == origin, and
// from == dest is never queried (being there already means arrived).
fn u64 pack_route_key(CellPos from, CellPos dest) {
    u64 key = ((u64)pack_cell(from) << 32) | (u64)pack_cell(dest);
    ASSERT(key != 0);
    return key;
}

struct NextHop {
    CellPos cell;
    b32     found;
};

// Next cell on a shortest passable path from -> dest. Cache-first; a miss
// runs BFS (4-way, uniform cost, passable cells only, early exit at dest) and
// installs next-hops for EVERY path cell toward dest, so one search feeds the
// whole journey and every follower on the same road. Unreachable, unwalkable
// endpoints, or a missing grid return found == false — the caller cancels.
fn NextHop route_next_hop(SpatialMap* spatial, const WorldMap* map, CellPos from, CellPos dest) {
    ASSERT(!(from == dest));
    if (!spatial->bfs_came_from || !in_bounds(map, from) || !in_bounds(map, dest)) return {};

    u64 key = pack_route_key(from, dest);
    if (u32* cached = hashtable::get(&spatial->next_hop_cache, key)) { return {unpack_cell(*cached), true}; }

    if (!TERRAIN_TYPES[(*map)[from].terrain_type_idx].passable) return {};
    if (!TERRAIN_TYPES[(*map)[dest].terrain_type_idx].passable) return {};

    usize area = (usize)map->width * (usize)map->height;
    memset(spatial->bfs_came_from, 0, area * sizeof(u32));
    u32   from_idx                   = (u32)cell_index(map, from);
    u32   dest_idx                   = (u32)cell_index(map, dest);
    usize head                       = 0;
    usize tail                       = 0;
    spatial->bfs_queue[tail++]       = from_idx;
    spatial->bfs_came_from[from_idx] = from_idx + 1; // self-pred marks visited
    b32 reached                      = false;
    while (head < tail && !reached) {
        u32     cur      = spatial->bfs_queue[head++];
        CellPos cur_pos  = cell_from_index(map, cur);
        CellPos steps[4] = {
            {cur_pos.x + 1, cur_pos.y},
            {cur_pos.x - 1, cur_pos.y},
            {cur_pos.x, cur_pos.y + 1},
            {cur_pos.x, cur_pos.y - 1},
        };
        for (CellPos step : steps) {
            if (!in_bounds(map, step)) continue;
            u32 step_idx = (u32)cell_index(map, step);
            if (spatial->bfs_came_from[step_idx]) continue;
            if (!TERRAIN_TYPES[(*map)[step].terrain_type_idx].passable) continue;
            spatial->bfs_came_from[step_idx] = cur + 1;
            if (step_idx == dest_idx) {
                reached = true;
                break;
            }
            spatial->bfs_queue[tail++] = step_idx;
        }
    }
    if (!reached) return {};

    // Install (cell, dest) -> next for the whole path. Purge-before-batch:
    // the capacity chosen at spatial_initialize guarantees a purged table
    // fits any path, so the cache never grows (tripwired below).
    usize path_edges = 0;
    for (u32 at = dest_idx; at != from_idx; at = spatial->bfs_came_from[at] - 1) {
        path_edges++;
    }
    if (hashtable::would_grow(&spatial->next_hop_cache, path_edges)) { hashtable::clear(&spatial->next_hop_cache); }
    usize capacity_before = spatial->next_hop_cache.capacity;
    u32   toward          = dest_idx;
    for (u32 at = spatial->bfs_came_from[dest_idx] - 1;; at = spatial->bfs_came_from[at] - 1) {
        CellPos at_pos = cell_from_index(map, at);
        *hashtable::put(&spatial->next_hop_cache, pack_route_key(at_pos, dest)) =
            pack_cell(cell_from_index(map, toward));
        if (at == from_idx) break;
        toward = at;
    }
    ASSERT(spatial->next_hop_cache.capacity == capacity_before);
    (void)capacity_before;

    u32* hop = hashtable::get(&spatial->next_hop_cache, key);
    ASSERT(hop);
    return {unpack_cell(*hop), true};
}

// --- Orders and movement -----------------------------------------------------

// Unordered pair of live slots; slots >= 1, so the key is never 0.
fn u64 pack_pair(u16 a, u16 b) {
    ASSERT(a != 0 && b != 0 && a != b);
    return ((u64)min(a, b) << 16) | (u64)max(a, b);
}

// Issue a move order. LOG + refuse movers that can never walk (dead, dummy,
// bodiless, immobile body). The dest's liveness is only enforced at tick time
// — it can despawn later anyway. dest = {} clears the order.
fn void order_move(World* world, Id mover, Id dest) {
    Thing* thing = get(world, mover);
    if (!thing->id) {
        LOG("order_move: dead or nil mover");
        return;
    }
    if (!dest) {
        thing->move_dest   = {};
        thing->move_points = 0;
        return;
    }
    String name = resolve_name(world, thing->name);
    if (thing->body_kind_idx == 0 || BODY_KINDS[thing->body_kind_idx].speed <= 0) {
        LOG("order_move: '%.*s' cannot move (bodiless or immobile)", (int)name.len, name.data);
        return;
    }
    thing->move_dest = dest;
}

// One intended step, produced by movement_intent, executed by movement_step.
// Whether the step ARRIVES is deliberately not computed here: the destination
// may itself move while the day's steps execute, so arrival compares against
// the dest's cell at step time — an intent-time flag would end a chase at the
// cell the target already left.
struct MoveIntent {
    u16     slot;
    CellPos to;      // the cell this mover steps onto (or surfaces onto, for exits)
    b32     is_exit; // the step is exit_to_map instead of move_thing
};

// The per-mover half of a movement day: validate the order, accrue points,
// pick the next hop toward the goal. Mutates only the thing's own order
// fields — this is what makes the intent loop parallelizable someday. (One
// caveat for a future parallel version: route_next_hop writes the shared
// next-hop cache.) Speeds are capped at one cell per day, so one intent
// covers the mover's whole day; {} means no step today (ZII — live slots
// are >= 1).
fn MoveIntent movement_intent(SpatialMap* spatial, World* world, Thing* thing) {
    if (!get(world, thing->move_dest)->id) {
        String name = resolve_name(world, thing->name);
        LOG("move: destination of '%.*s' is gone — order canceled", (int)name.len, name.data);
        thing->move_dest   = {};
        thing->move_points = 0;
        return {};
    }
    Thing* goal_anchor = resolve_map_anchor(world, thing->move_dest);
    if (!goal_anchor->id) {
        String name = resolve_name(world, thing->name);
        LOG("move: destination of '%.*s' is nowhere on the map — order canceled", (int)name.len, name.data);
        thing->move_dest   = {};
        thing->move_points = 0;
        return {};
    }
    CellPos goal = goal_anchor->cell_pos;

    // Exiting the container is the day's movement, ungated by points.
    if (!is_on_map(world, thing)) {
        Thing* anchor = resolve_map_anchor(world, thing->id);
        if (!anchor->id) {
            String name = resolve_name(world, thing->name);
            LOG("move: '%.*s' is nowhere on the map and cannot exit — order canceled", (int)name.len, name.data);
            thing->move_dest   = {};
            thing->move_points = 0;
            return {};
        }
        return {thing->id.slot, anchor->cell_pos, true};
    }

    thing->move_points = min(thing->move_points + BODY_KINDS[thing->body_kind_idx].speed, 1.0f);
    if (thing->move_points < 1.0f) return {};

    if (thing->cell_pos == goal) {
        return {thing->id.slot, thing->cell_pos, false}; // zero-length arrival
    }
    NextHop hop = route_next_hop(spatial, &world->world_map, thing->cell_pos, goal);
    if (!hop.found) {
        String name = resolve_name(world, thing->name);
        LOG("move: no road takes '%.*s' from %d,%d to %d,%d — order canceled", (int)name.len, name.data,
            thing->cell_pos.x, thing->cell_pos.y, goal.x, goal.y);
        thing->move_dest   = {};
        thing->move_points = 0;
        return {};
    }
    return {thing->id.slot, hop.cell, false};
}

// Day-scoped state for the step loop. Firing rules: at most once per pair per
// day (fired), and pairs already sharing a cell at day start don't re-fire
// while together (companions).
struct MoveDay {
    hashtable::Hashtable<b32> together;
    hashtable::Hashtable<b32> fired;
    b32                       logged_full;
};

// Opens a movement day: clears the event buffer and snapshots day-start
// co-location, so it must run after the intent loop and before the first
// movement_step. Walking each slot's successors visits every unordered pair
// exactly once (unlinked slots chain to 0 for free).
fn MoveDay movement_day_begin(Game* game, Arena* scratch) {
    World*      world   = &game->world;
    SpatialMap* spatial = &game->spatial;
    clear(&game->events);

    MoveDay day  = {};
    day.together = hashtable::make_table<b32>(scratch, 64);
    day.fired    = hashtable::make_table<b32>(scratch, 64);
    for (usize slot = 1; slot <= world->high_water; slot++) {
        if (!world->things[slot].id) continue;
        for (u16 at = spatial->cell_next[slot]; at; at = spatial->cell_next[at]) {
            *hashtable::put(&day.together, pack_pair((u16)slot, at)) = true;
        }
    }
    return day;
}

// The sequential half: execute one intent against the live grid, so every
// encounter check reads true current positions. Step order is slot order —
// the deterministic tiebreak for genuine races. (An equal-speed chase never
// fires when the fleer's slot precedes the chaser's; chaser-first firing a
// Meet is correct sequential semantics — it genuinely entered a still-
// occupied cell.)
fn void movement_step(Game* game, MoveDay* day, MoveIntent intent) {
    auto* world   = &game->world;
    auto* spatial = &game->spatial;
    auto* thing   = &world->things[intent.slot];
    // Intent and step happen the same tick, but the re-check is cheap and
    // keeps the step safe against anything that cancels orders in between.
    if (!thing->id || !thing->move_dest) return;

    b32 stepped     = false;
    Id  exited_from = {};
    if (intent.is_exit) {
        // The on-map thing whose cell we surface at — the one occupant an
        // exit must NOT read as a meeting (nested containment makes this
        // the outermost container, not the direct parent).
        exited_from = resolve_map_anchor(world, thing->id)->id;
        if (!exit_to_map(spatial, world, thing)) {
            thing->move_dest   = {};
            thing->move_points = 0;
            return;
        }
        stepped = true;
    } else if (!(intent.to == thing->cell_pos)) {
        move_thing(spatial, &world->world_map, thing, intent.to);
        thing->move_points -= 1.0f;
        stepped = true;
    }

    // A step into (or onto) a cell meets everyone standing there now — except
    // the container just left: surfacing out of a town is not a meeting with
    // it.
    if (stepped) {
        auto here = thing->cell_pos;
        for (auto* other : occupants_of(spatial, world, here)) {
            if (other->id.slot == thing->id.slot) continue;
            if (exited_from && other->id.slot == exited_from.slot) continue;
            u64 pair = pack_pair(thing->id.slot, other->id.slot);
            if (hashtable::get(&day->together, pair) || hashtable::get(&day->fired, pair)) continue;
            *hashtable::put(&day->fired, pair) = true;
            Event event                        = {};
            event.kind                         = EventKind::Meet;
            event.a                            = thing->id;
            event.b                            = other->id;
            event.cell                         = here;
            if (!push(&game->events, event) && !day->logged_full) {
                LOG("movement: event buffer full — records dropped");
                day->logged_full = true;
            }
        }
    }

    // Arrival is orthogonal to any meeting the same step produced: it
    // resolves the ORDER, not the pair. The goal re-resolves HERE, against
    // the dest's current cell — a chaser that steps onto the cell its
    // target just left has not arrived.
    auto* goal_anchor = resolve_map_anchor(world, thing->move_dest);
    if (goal_anchor->id && thing->cell_pos == goal_anchor->cell_pos) {
        Event event = {};
        event.kind  = EventKind::Arrival;
        event.a     = thing->id;
        event.b     = thing->move_dest;
        event.cell  = thing->cell_pos;
        if (!push(&game->events, event) && !day->logged_full) {
            LOG("movement: event buffer full — records dropped");
            day->logged_full = true;
        }
        thing->move_dest   = {};
        thing->move_points = 0;
    }
}

// Placeholder consumption until the interaction system exists: the log is
// the observer. Records stay in place for tests to inspect.
fn void event_log(World* world, Event event) {
    String a = resolve_name(world, get(world, event.a)->name);
    String b = resolve_name(world, get(world, event.b)->name);
    switch (event.kind) {
        case EventKind::Nil: break;
        case EventKind::Meet:
            LOG("encounter: '%.*s' met '%.*s' at %d,%d", (int)a.len, a.data, (int)b.len, b.data, event.cell.x,
                event.cell.y);
            break;
        case EventKind::Arrival:
            LOG("arrival: '%.*s' reached '%.*s' at %d,%d", (int)a.len, a.data, (int)b.len, b.data, event.cell.x,
                event.cell.y);
            break;
    }
}

// --- Validation ---------------------------------------------------------------

// LOG-based world sanity pass — data errors are expected failures, so this
// never traps. Returns the problem count (tests CHECK == 0). The zero Game
// reads as clean (ZII).
fn u32 validate(Game* game) {
    World*          world    = &game->world;
    const WorldMap* map      = &world->world_map;
    SpatialMap*     spatial  = &game->spatial;
    u32             problems = 0;

    for (usize idx = 1; idx < BODY_KINDS.len(); idx++) {
        if (BODY_KINDS[idx].speed > 1.0f) {
            LOG("validate: body kind '%.*s' speed %f exceeds one cell per day", (int)BODY_KINDS[idx].name.len,
                BODY_KINDS[idx].name.data, (f64)BODY_KINDS[idx].speed);
            problems++;
        }
    }

    usize on_map_count = 0;
    for (usize slot = 1; slot <= world->high_water; slot++) {
        const Thing* thing = &world->things[slot];
        if (!thing->id) continue;
        String name = resolve_name(world, thing->name);

        if (thing->move_dest && !get(world, thing->move_dest)->id) {
            LOG("validate: '%.*s' has a dead move destination", (int)name.len, name.data);
            problems++;
        }
        if (!is_on_map(world, thing)) continue;
        on_map_count++;

        if (!in_bounds(map, thing->cell_pos)) {
            LOG("validate: '%.*s' is off the map at %d,%d", (int)name.len, name.data, thing->cell_pos.x,
                thing->cell_pos.y);
            problems++;
            continue;
        }
        const TerrainType* terrain = &TERRAIN_TYPES[(*map)[thing->cell_pos].terrain_type_idx];
        if (!terrain->passable) {
            LOG("validate: '%.*s' stands on impassable %.*s at %d,%d", (int)name.len, name.data, (int)terrain->name.len,
                terrain->name.data, thing->cell_pos.x, thing->cell_pos.y);
            problems++;
        }
        if (spatial->cell_heads) {
            u32   seen   = 0;
            usize walked = 0;
            for (u16 at = spatial->cell_heads[cell_index(map, thing->cell_pos)]; at; at = spatial->cell_next[at]) {
                if (at == thing->id.slot) seen++;
                if (++walked > NUM_ENTITIES) break; // cycle: reported by the chain audit below
            }
            if (seen != 1) {
                LOG("validate: '%.*s' appears %d times in its cell's chain", (int)name.len, name.data, (int)seen);
                problems++;
            }
        }
    }

    if (spatial->cell_heads) {
        usize chained = 0;
        usize area    = (usize)map->width * (usize)map->height;
        for (usize cell = 0; cell < area; cell++) {
            usize walked = 0;
            for (u16 at = spatial->cell_heads[cell]; at; at = spatial->cell_next[at]) {
                if (++walked > NUM_ENTITIES) {
                    LOG("validate: chain cycle at cell %d", (int)cell);
                    problems++;
                    break;
                }
                const Thing* thing = &world->things[at];
                if (!thing->id || !is_on_map(world, thing) || !in_bounds(map, thing->cell_pos) ||
                    cell_index(map, thing->cell_pos) != cell) {
                    LOG("validate: chained slot %d does not belong at cell %d", (int)at, (int)cell);
                    problems++;
                }
                chained++;
            }
        }
        if (chained != on_map_count) {
            LOG("validate: %d chained things but %d on-map things", (int)chained, (int)on_map_count);
            problems++;
        }
    }
    return problems;
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

// Returns true if the game wants to be paused, with no other event happening. Ticks will use this
// and disregard all kinds of command. The UI should act accordingly
fn b32 forced_pause(const Game* game) { return game->world.interaction.active; }

fn void tick(Game* game, TickCommands commands) {
    if (forced_pause(game)) { return; }

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
        if (advance_day) {
            world->epoch++;
            ScratchArena scratch(&game->arena);

            auto intents = vec::make_vec<MoveIntent>(&game->arena, 64);

            for (usize slot = 1; slot <= world->high_water; slot++) {
                Thing* thing = &world->things[slot];
                if (!thing->id || !thing->move_dest) continue;
                MoveIntent intent = movement_intent(&game->spatial, world, thing);
                if (intent.slot) vec::push(&intents, intent);
            }

            MoveDay day = movement_day_begin(game, &game->arena);
            for (MoveIntent& intent : vec::slice(intents)) {
                movement_step(game, &day, intent);
            }

            for (Event& event : game->events) {
                if (event.kind == EventKind::Meet) {
                    world->interaction.active      = true;
                    world->interaction.title       = "Test title";
                    world->interaction.description = "This is a test event";

                    add_choice(&world->interaction, "Choice A", true);
                    add_choice(&world->interaction, "Choice B", true);
                    add_choice(&world->interaction, "Choice C", false);
                }
                event_log(world, event);
            }
        }

        despawn_flush(&game->spatial, world);

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

    if (world->interaction.active) {
        ui::data::bind_global(&data, "INTERACTION", "yes");
        ui::data::bind_global(&data, "INT_TITLE", arena::clone_string(arena, world->interaction.title));
        ui::data::bind_global(&data, "INT_TEXT", arena::clone_string(arena, world->interaction.description));
        ui::data::begin_list(&data, "choices");
        usize idx = 0;
        for (auto& choice : world->interaction.choices) {
            ui::data::begin_row(&data);
            ui::data::bind(&data, "INDEX", string::from_int(arena, (i64)idx));
            ui::data::bind(&data, "CHOICE", arena::clone_string(arena, choice.text));
            ui::data::bind(&data, "ENABLED", choice.enabled ? "yes" : "no");
            idx++;
        }
    }

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
        if (!thing.id) { continue; }
        // Contained things don't draw — the container is their visual.
        if (get_parent(world, &thing, Hierarchy::LocationOf)->id) { continue; }
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

    // Persistent spatial allocations come from the Game-owned arena — the
    // arena parameter is the FRAME arena and dies with the first frame.
    if (!game->arena.base) { arena::reserve(&game->arena, GAME_ARENA_SIZE); }
    spatial_initialize(&game->spatial, &world->world_map, &game->arena);

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

    // Spawned ids, parallel to the entity roots — the second pass below walks
    // the roots in the same order to wire cross-entity references by name.
    auto spawned = vec::make_vec<Id>(arena, 64);
    for (const auto& root : result.roots) {
        if (root.key == "entity") {
            auto* thing = spawn(world);
            vec::push(&spawned, thing->id);
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

    // Second pass: fields that reference other entities by name, resolvable
    // only once everything is spawned.
    usize entity_idx = 0;
    for (const auto& root : result.roots) {
        if (root.key != "entity") continue;
        if (entity_idx >= spawned.len) break;
        Id mover = spawned[entity_idx++];

        String target = tabula::get_text(&root, "move_to");
        if (!target.len || !mover) continue;
        Id dest = find_thing_by_name(world, target);
        if (!dest) {
            LOG("setup: move_to target '%.*s' not found", (int)target.len, target.data);
            continue;
        }
        order_move(world, mover, dest);
    }

    // Mutations stop here, same contract as tick: indexes fresh before the
    // first frame reads, and the validation pass reports data that makes no
    // sense (things off-road, dead references).
    spatial_rebuild(&game->spatial, world);
    hierarchy_refresh(game);
    validate(game);
}

} // namespace game
