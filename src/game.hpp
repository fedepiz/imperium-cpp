#pragma once

#include "core.hpp"
#include "math.hpp"
#include "string.hpp"
#include "arena.hpp"
#include "tabula.hpp"
#include "ui/data.hpp"
#include "vec.hpp"

namespace game {
using namespace arena;

struct Thing;

// Generational entity id;
struct Id {
    u16 slot;
    u16 generation;

    operator b32() const;
};

constexpr Id NIL = {};

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
};

constexpr usize NUM_ENTITIES    = 32000;
constexpr usize NAME_BUFFER_LEN = 1024 * 1024;

// Authoritative game state. ZII: the all-zero world is valid and empty —
// slot 0 is the permanently-zeroed dummy, the first spawn takes slot 1.
struct World {
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

    {
        ui::data::bind_global(&data, "INTERACTION", "yes");
        ui::data::bind_global(&data, "INT_TITLE", "Title goes here");
        ui::data::bind_global(&data, "INT_TEXT", "The description would go here");
        ui::data::begin_list(&data, "choices");
        ui::data::begin_row(&data);
        ui::data::bind(&data, "INDEX", "1");
        ui::data::bind(&data, "CHOICE", "Yes");
        ui::data::begin_row(&data);
        ui::data::bind(&data, "INDEX", "2");
        ui::data::bind(&data, "CHOICE", "No");
    }
    return data;
}

struct MapItem {
    math::Rect  bounds;
    math::Color color;
};

struct DrawMap {
    Slice<MapItem> items;
};

fn DrawMap draw_map(Arena* arena, const World* world) {
    DrawMap out = {};
    // Some test items
    auto items = vec::make_vec<MapItem>(arena, 100);
    auto size  = math::splat(60);
    vec::push(&items, MapItem{
                          .bounds = math::rect_with_center_and_size({100, 100}, size),
                          .color  = math::RED,
                      });
    vec::push(&items, MapItem{
                          .bounds = math::rect_with_center_and_size({200, 200}, size),
                          .color  = math::BLUE,
                      });
    out.items = vec::slice(items);
    return out;
}

} // namespace game
