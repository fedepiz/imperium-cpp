#pragma once
#include <errno.h>
#include <stdio.h>

#include "core.hpp"
#include "arena.hpp"
#include "list.hpp"
#include "string.hpp"

// Whole-file reads into an arena, on portable C stdio — no OS-specific APIs,
// so there are no platform blocks. ZII: on failure the data slice stays zero
// and `messages` says why — success is exactly `messages.len == 0`. An empty
// file is a successful read of an empty slice. Everything in the result
// (contents, error text) lives in the arena passed in.
//
// Sizes go through fseek/ftell, so files cap at LONG_MAX — 2GB where long is
// 32 bits. Fine for asset files.

namespace file_io {

enum class ErrorKind : u32 {
    Nil, // zero — no error
    NotFound,
    AccessDenied,
    OpenFailed, // open/size failures that aren't the two above
    ReadFailed,
};

struct Error {
    ErrorKind kind;
    String    message; // "path: cause", arena-allocated
};

template <typename T> struct ReadFile {
    T                 data;
    list::List<Error> messages; // empty = success
};

fn Error make_error(arena::Arena* arena, ErrorKind kind, String path, const char* cause) {
    usize cause_len = strlen(cause);
    char* text      = (char*)arena::allocate_raw(arena, path.len + 2 + cause_len, 1);
    if (path.len) memcpy(text, path.data, path.len);
    memcpy(text + path.len, ": ", 2);
    memcpy(text + path.len + 2, cause, cause_len);

    Error error   = {};
    error.kind    = kind;
    error.message = {path.len + 2 + cause_len, text};
    return error;
}

fn ReadFile<Slice<u8>> read_file_to_bytes(arena::Arena* arena, String path) {
    ReadFile<Slice<u8>> result = {};
    result.messages            = list::make_list<Error>(arena);

    FILE* file = fopen(string::to_cstr(arena, path), "rb");
    if (!file) {
        ErrorKind kind = errno == ENOENT   ? ErrorKind::NotFound
                         : errno == EACCES ? ErrorKind::AccessDenied
                                           : ErrorKind::OpenFailed;
        list::push(&result.messages, make_error(arena, kind, path, strerror(errno)));
        return result;
    }

    long size_or_error = -1;
    if (fseek(file, 0, SEEK_END) == 0) size_or_error = ftell(file);
    if (size_or_error < 0) {
        list::push(&result.messages, make_error(arena, ErrorKind::OpenFailed, path, strerror(errno)));
        fclose(file);
        return result;
    }
    rewind(file);

    usize size  = (usize)size_or_error;
    u8*   data  = arena::allocate_raw(arena, size, 1);
    usize total = size ? fread(data, 1, size, file) : 0;
    if (total != size && ferror(file)) {
        list::push(&result.messages, make_error(arena, ErrorKind::ReadFailed, path, strerror(errno)));
        fclose(file);
        return result;
    }
    fclose(file);

    result.data = {total, data}; // short-but-clean read: the file shrank under us, keep what arrived
    return result;
}

fn ReadFile<String> read_file_to_string(arena::Arena* arena, String path) {
    ReadFile<Slice<u8>> bytes = read_file_to_bytes(arena, path);

    ReadFile<String> result = {};
    result.data             = {bytes.data.len, (char*)bytes.data.data};
    result.messages         = bytes.messages;
    return result;
}

} // namespace file_io
