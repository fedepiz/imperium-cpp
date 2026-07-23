#pragma once
#include "arena.hpp"
#include "core.hpp"
#include "string.hpp"
#include "vec.hpp"

// Key/value sheet: one global row of bindings plus named lists of rows.
// Keys (and String values) are cloned into the arena; readers get views into it.
//
//     sheet::Sheet s = sheet::make(&frame_arena);
//
//     sheet::bind_global(&s, "name", "Roma");
//     sheet::bind_global(&s, "at_war", true);
//
//     sheet::begin_list(&s, "legions");           // items go to the newest list
//     sheet::bind_item(&s, "name", "XIII Gemina"); // first bind_item opens an item
//     sheet::bind_item(&s, "veteran", true);
//     sheet::end_item(&s);                         // close it; next bind_item starts a new one
//     sheet::bind_item(&s, "name", "IX Hispana");
//
//     sheet::Row global = sheet::get_global(s);
//     String name   = sheet::get_str(global, "name");    // "Roma"
//     b32    at_war = sheet::get_bool(global, "at_war"); // true
//     for (sheet::Row row : sheet::get_list(s, "legions")) {
//         String legion  = sheet::get_str(row, "name");
//         b32    veteran = sheet::get_bool(row, "veteran");
//     }

namespace sheet {
struct Binding {
    String key;
    String value;
};

using Row = vec::Vec<Binding>;

struct List {
    String        key;
    vec::Vec<Row> rows;
};

struct Sheet {
    arena::Arena*  arena;
    Row            global_row;
    vec::Vec<List> lists;
};

namespace impl {
fn void push_binding(arena::Arena* arena, Row* row, String key, String value, bool copy) {
    if (!row) return;
    key = arena::clone_string(arena, key);
    if (copy) { value = arena::clone_string(arena, value); }
    vec::push(row, {key, value});
}

fn Row* active_list_row(Sheet* sheet) {
    auto* active_list = vec::last(sheet->lists);
    if (!active_list) return nullptr;

    if (active_list->rows.len == 0) { vec::push(&active_list->rows, vec::make<Binding>(sheet->arena, 0)); }

    return vec::last(active_list->rows);
}

fn String to_repr(b32 value) { return value ? "yes" : "no"; }

fn b32 from_repr(String value) {
    if (value.len == 0) { return false; }
    if (value == "yes") {
        return true;
    } else if (value == "no") {
        return false;
    } else {
        LOG("Invalid b32 representation ", value);
        return false;
    }
}

} // namespace impl

fn Sheet make(arena::Arena* arena) {
    Sheet out      = {};
    out.arena      = arena;
    out.global_row = vec::make<Binding>(arena, 0);
    out.lists      = vec::make<List>(arena, 0);
    return out;
}

fn void bind_global(Sheet* sheet, String key, String value) {
    impl::push_binding(sheet->arena, &sheet->global_row, key, value, true);
}

fn void bind_global(Sheet* sheet, String key, b32 value) {
    impl::push_binding(sheet->arena, &sheet->global_row, key, impl::to_repr(value), false);
}

fn void bind_item(Sheet* sheet, String key, String value) {
    auto* row = impl::active_list_row(sheet);
    impl::push_binding(sheet->arena, row, key, value, true);
}

fn void bind_item(Sheet* sheet, String key, b32 value) {
    auto* row = impl::active_list_row(sheet);
    impl::push_binding(sheet->arena, row, key, impl::to_repr(value), false);
}

fn void end_item(Sheet* sheet) {
    auto* active_list = vec::last(sheet->lists);
    if (active_list) {
        auto row = vec::make<Binding>(sheet->arena, 0);
        vec::push(&active_list->rows, row);
    }
}

fn void begin_list(Sheet* sheet, String key) {
    List list = {};
    list.key  = key;
    list.rows = vec::make<Row>(sheet->arena, 0);
    vec::push(&sheet->lists, list);
}

fn String get_str(Row row, String key) {
    for (const auto& entry : row) {
        if (entry.key == key) { return entry.value; }
    }
    return {};
}

fn b32 get_bool(Row row, String key) {
    auto value = get_str(row, key);
    return impl::from_repr(value);
}

fn Slice<Row> get_list(Sheet sheet, String key) {
    for (const auto& list : sheet.lists) {
        if (list.key == key) { return vec::slice(list.rows); }
    }
    return {};
}

fn Row get_global(Sheet sheet) { return sheet.global_row; }

} // namespace sheet
