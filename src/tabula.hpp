#pragma once
#include "core.hpp"
#include "arena.hpp"
#include "string.hpp"
#include "vec.hpp"

namespace tabula {
struct Value {
    String text;
    b32    has_number;
    f32    number;
};

struct Node {
    String      key;
    Value       value;
    Slice<Node> children;
};

struct Error {
    String message;
    u32    col;
    u32    line;
};

struct Parse {
    Node         root;
    Slice<Error> errors;
};

namespace parsing {
struct Context {
    arena::Arena*   arena;
    String          source;
    usize cursor;
    u32             col;
    u32             line;
    vec::Vec<Error> errors;
    vec::Vec<Node>  open_children;
};

Node close_node(Context* ctx) {
    Node node     = {};
    node.children = arena::make_slice<Node>(ctx->arena, ctx->open_children.len);
    for (usize i = 0; i < ctx->open_children.len; ++i) {
        node.children[i] = ctx->open_children[i];
    }
    vec::clear(&ctx->open_children);
    return node;
}

Node parse_node(Context* ctx) {
    // First parse the node's key
    return close_node(ctx);
}

Parse parse(Context* ctx) {
    // Perform the actual parsing here...
    Parse out  = {};
    out.root   = close_node(ctx);
    out.errors = vec::slice(ctx->errors);
    return out;
}
} // namespace parsing

Parse parse(arena::Arena* arena, String source) {
    parsing::Context ctx = {};
    ctx.arena            = arena;
    ctx.source           = source;
    ctx.errors           = vec::make_vec<Error>(arena, 0);
    return parsing::parse(&ctx);
}
}; // namespace tabula
