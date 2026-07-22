#pragma once
#include "../core.hpp"
#include "../arena.hpp"
#include "../vec.hpp"
#include "layout.hpp"

// The data the UI binds against, one struct per frame fill: the caller
// fetches values from wherever it likes and hands them over in this format,
// keeping the UI isolated from the rest of the game. ui::run interpolates
// the script's $VARs against it.

namespace ui {
namespace data {
using layout::ImageId;

// One $VAR binding: key is the variable name without the '$'. The strings
// are the caller's — typically literals or frame-arena formatted text — and
// must outlive the frame; Data references, it does not copy.
struct Binding {
    String key;
    String value;
};

// The bindings one stamped-out template instance interpolates from: a range
// into Data::bindings. Indices rather than a Slice because the vec grows
// (and may relocate) while later rows are still being filled.
struct Row {
    u32 first_binding;
    u32 binding_count;
};

// The rows behind one list, matched to it by id: a range into Data::rows.
struct ListData {
    String id;
    u32    first_row;
    u32    row_count;
};

// One image the script can reference by key (image = { source = soldier }),
// with its renderer handle and natural size.
struct ImageData {
    String  key;
    ImageId image;
    f32     width;
    f32     height;
};

// Everything the UI binds against this frame: flat arrays, cross-linked by
// index ranges, strings referencing the caller's memory. The zero value is
// valid (no lists, no images); clear recycles the buffers so one value can
// be refilled every frame without reallocating.
//
// One channel carries all sim-facing data: bindings, resolved current row
// first, then globals — the root scope every element sees, including
// top-level panels outside any list. Visibility rides the same channel
// (visible = "$OPEN" against a yes/no value).
//
// Built top-down in declaration order: begin_list, then begin_row and bind
// for each row, so every list's rows (and every row's bindings) are
// contiguous. Globals live in their own array, so bind_global is legal at
// any point.
struct Data {
    vec::Vec<ListData>  lists;
    vec::Vec<Row>       rows;
    vec::Vec<Binding>   bindings;
    vec::Vec<Binding>   globals;
    vec::Vec<ImageData> images;
};

fn Data make(arena::Arena* arena) {
    Data data           = {};
    data.lists.arena    = arena;
    data.rows.arena     = arena;
    data.bindings.arena = arena;
    data.globals.arena  = arena;
    data.images.arena   = arena;
    return data;
}

fn void clear(Data* data) {
    vec::clear(&data->lists);
    vec::clear(&data->rows);
    vec::clear(&data->bindings);
    vec::clear(&data->globals);
    vec::clear(&data->images);
}

// Starts a new list; subsequent begin_row calls belong to it.
fn void begin_list(Data* data, String id) { vec::push(&data->lists, ListData{id, (u32)data->rows.len, 0}); }

// Starts a new row in the current list; subsequent bind calls belong to it.
// Without a begin_list first, the row is orphaned (harmless: nothing ranges
// over it).
fn void begin_row(Data* data) {
    vec::push(&data->rows, Row{(u32)data->bindings.len, 0});
    if (data->lists.len) data->lists[data->lists.len - 1].row_count += 1;
}

// Adds a $key = value binding to the current row. Without a begin_row
// first, the binding is orphaned (harmless).
fn void bind(Data* data, String key, String value) {
    vec::push(&data->bindings, Binding{key, value});
    if (data->rows.len) data->rows[data->rows.len - 1].binding_count += 1;
}

fn void bind(Data* data, String key, b32 value) {
    String text = value ? "yes" : "no";
    bind(data, key, text);
}

// Adds a $key = value binding to the root scope: visible to every element,
// shadowed by a row binding of the same key. Legal at any point during the
// fill — globals live outside the row/list ranges.
fn void bind_global(Data* data, String key, String value) { vec::push(&data->globals, Binding{key, value}); }

fn void bind_global(Data* data, String key, b32 value) {
    String text = value ? "yes" : "no";
    bind_global(data, key, text);
}

fn void add_image(Data* data, String key, ImageId image, f32 width, f32 height) {
    vec::push(&data->images, ImageData{key, image, width, height});
}

fn Slice<Row> rows(const Data* data, ListData list) { return {list.row_count, data->rows.data + list.first_row}; }

fn Slice<Binding> bindings(const Data* data, Row row) {
    return {row.binding_count, data->bindings.data + row.first_binding};
}

} // namespace data
} // namespace ui
