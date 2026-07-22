#pragma once
#include "core.hpp"
#include "arena.hpp"
#include "string.hpp"
#include "vec.hpp"

// Structural parser for tabula, a Clausewitz-style data format:
//
//     legion = {
//         name = "Legio I"        # comments run to end of line
//         strength = 4800
//         cohorts = { 1 2 3 }     # blocks can be array-like
//         morale > 0.5            # comparison operators
//     }
//     legion = { name = "Legio II" }   # duplicate keys are legal
//
// The parser is purely structural: keys, operators, atoms (scalars and
// quoted strings), and nested blocks. No key has special meaning.
//
// Everything lives in the arena passed to parse(): node storage, child
// slices, error storage, and the key/value strings (copied out of the
// source). The tree references only the arena — the source buffer can be
// discarded as soon as parse() returns — and is freed with the arena.
//
// Parsing never fails: parse() always returns a ParseResult holding whatever
// tree could be recovered plus every error encountered.

namespace tabula {

// What a node is. Zero value = Atom — with an empty value, that is the ZII
// "nothing" node.
enum class Kind : u32 {
    Atom,  // scalar or quoted string; the text (and number, if numeric) is in value
    Block, // { ... }; contents are in children, value is empty
};

// Operator between key and value. Zero value = Nil, meaning the node is a
// bare value with no key (an array element).
enum class Op : u32 {
    Nil,
    Eq, // =
    Lt, // <
    Gt, // >
    Le, // <=
    Ge, // >=
    Ne, // !=
};

// An atom's value, fat-struct style: always the raw text — preserving the
// source spelling ("4800" vs "4.8e3") — plus the parsed number when an
// unquoted atom is numeric. Quoted atoms are never numbers: "12" stays text.
// Zero value = empty non-number.
struct Value {
    b32    is_number;
    String text;
    f32    number; // zero when is_number is false
};

// One fat struct covers every syntactic form; unused fields stay zero.
//
// | form            | key   | op     | kind    | value.text | children |
// |-----------------|-------|--------|---------|------------|----------|
// | a = 1           | "a"   | Eq     | Atom    | "1"        | empty    |
// | a = "x"         | "a"   | Eq     | Atom    | "x"        | empty    |
// | a = { ... }     | "a"   | Eq     | Block   | ""         | items    |
// | a > 5           | "a"   | Gt     | Atom    | "5"        | empty    |
// | bare 1          | ""    | Nil    | Atom    | "1"        | empty    |
// | bare { ... }    | ""    | Nil    | Block   | ""         | items    |
struct Node {
    String      key;
    Op          op;
    Kind        kind;
    Value       value;
    Slice<Node> children;
};

enum class ErrorKind : u32 {
    Nil, // zero: no error
    UnexpectedChar,
    UnclosedString,
    UnclosedBlock,
    UnexpectedCloseBrace,
    ExpectedValue,
    TooDeep,
    UnexpectedComma, // commas are not separators; whitespace is
};

struct ParseError {
    ErrorKind kind;
    usize     offset;  // byte offset into the source
    u32       line;    // 1-based
    u32       col;     // 1-based, in bytes
    String    message; // "line:col: description", arena-allocated; prefix the path yourself
};

// What parse() produces: the recovered tree plus every error, in one flat
// struct. Zero value = no roots, no errors.
//
// roots are the file's top-level items — a file is a sequence of them, not a
// single node. On errors the parser recovers and keeps going, so roots holds
// whatever could still be parsed (possibly nothing).
struct ParseResult {
    Slice<Node>       roots;
    Slice<ParseError> errors;
};

// Lookups on a missing key return this permanently-zeroed node, so chained
// gets are always safe to call and read.
inline const Node NIL_NODE = {};

constexpr u32 MAX_DEPTH = 500;

struct Parser {
    String               text;
    usize                pos;
    arena::Arena*        arena;
    vec::Vec<ParseError> errors;
};

// Bytes that end a bare scalar. All ASCII, so slicing at them always lands
// on a UTF-8 boundary. ',' terminates so "8," splits into an atom and a loud
// UnexpectedComma instead of parsing as a silent non-number.
fn bool is_terminator(char b) {
    return b == ' ' || b == '\t' || b == '\r' || b == '\n' || b == '{' || b == '}' || b == '=' || b == '<' ||
           b == '>' || b == '!' || b == '#' || b == '"' || b == ',';
}

fn bool is_space(char b) { return b == ' ' || b == '\t' || b == '\r' || b == '\n'; }

fn void skip_trivia(Parser* p) {
    while (p->pos < p->text.len) {
        char b = p->text.data[p->pos];
        if (is_space(b)) {
            p->pos += 1;
        } else if (b == '#') {
            while (p->pos < p->text.len) {
                char c = p->text.data[p->pos];
                p->pos += 1;
                if (c == '\n') break;
            }
        } else {
            return;
        }
    }
}

fn String error_description(ErrorKind kind) {
    switch (kind) {
    case ErrorKind::Nil: return "";
    case ErrorKind::UnexpectedChar: return "unexpected character";
    case ErrorKind::UnclosedString: return "unclosed string";
    case ErrorKind::UnclosedBlock: return "unclosed block";
    case ErrorKind::UnexpectedCloseBrace: return "unexpected '}'";
    case ErrorKind::ExpectedValue: return "expected a value";
    case ErrorKind::TooDeep: return "nesting too deep";
    case ErrorKind::UnexpectedComma: return "unexpected ',' — separate with whitespace";
    }
    return "";
}

fn void push_u32(vec::Vec<char>* text, u32 value) {
    char  digits[10]; // u32 fits in 10 decimal digits
    usize count = 0;
    do {
        digits[count] = (char)('0' + value % 10);
        count += 1;
        value /= 10;
    } while (value);
    while (count) {
        count -= 1;
        vec::push(text, digits[count]);
    }
}

fn ParseError make_error(Parser* p, ErrorKind kind) {
    u32 line = 1;
    u32 col  = 1;
    for (usize i = 0; i < p->pos; ++i) {
        if (p->text.data[i] == '\n') {
            line += 1;
            col = 1;
        } else {
            col += 1;
        }
    }

    vec::Vec<char> message = vec::make_vec<char>(p->arena, 0);
    push_u32(&message, line);
    vec::push(&message, ':');
    push_u32(&message, col);
    vec::push_all(&message, String{": "});
    vec::push_all(&message, error_description(kind));

    return {.kind = kind, .offset = p->pos, .line = line, .col = col, .message = vec::slice(message)};
}

fn void record(Parser* p, ErrorKind kind) { vec::push(&p->errors, make_error(p, kind)); }

// Skip past the offending input so parsing can continue: advance to the next
// whitespace or '}' (never consuming a '}', which may close an enclosing
// block).
fn void recover(Parser* p) {
    while (p->pos < p->text.len) {
        char b = p->text.data[p->pos];
        if (b == '}' || is_space(b)) return;
        p->pos += 1;
    }
}

// Term/item results, fat-struct style: error.kind == Nil means ok.
struct TermResult {
    Node       node;
    ParseError error;
};

// op == Op::Nil with a zero error means "no operator here".
struct OpResult {
    Op         op;
    ParseError error;
};

fn Value value_from_text(String text) {
    string::ParseF32Result number = string::parse_f32(text);
    return {.is_number = number.ok, .text = text, .number = number.value};
}

TermResult parse_item(Parser* p, u32 depth);

// Items until EOF or an unconsumed '}' (at depth 0 a stray '}' is recorded
// and skipped instead). Infallible: item errors are recorded and recovered
// from.
fn Slice<Node> parse_items(Parser* p, u32 depth) {
    vec::Vec<Node> items = vec::make_vec<Node>(p->arena, 0);
    while (true) {
        skip_trivia(p);
        if (p->pos >= p->text.len) break;
        if (p->text.data[p->pos] == '}') {
            if (depth > 0) break;
            record(p, ErrorKind::UnexpectedCloseBrace);
            p->pos += 1;
            continue;
        }
        TermResult item = parse_item(p, depth);
        if (item.error.kind != ErrorKind::Nil) {
            vec::push(&p->errors, item.error);
            recover(p);
            continue;
        }
        vec::push(&items, item.node);
    }
    return vec::slice(items);
}

// A keyless term: atom or block. key/op stay zero.
fn TermResult parse_term(Parser* p, u32 depth) {
    if (p->pos >= p->text.len) return {.error = make_error(p, ErrorKind::ExpectedValue)};
    char b = p->text.data[p->pos];
    if (b == '}') return {.error = make_error(p, ErrorKind::ExpectedValue)};
    if (b == ',') return {.error = make_error(p, ErrorKind::UnexpectedComma)};
    if (b == '=' || b == '<' || b == '>' || b == '!') return {.error = make_error(p, ErrorKind::UnexpectedChar)};

    if (b == '{') {
        if (depth >= MAX_DEPTH) return {.error = make_error(p, ErrorKind::TooDeep)};
        p->pos += 1;
        Slice<Node> children = parse_items(p, depth + 1);
        if (p->pos < p->text.len && p->text.data[p->pos] == '}') {
            p->pos += 1;
        } else {
            // EOF inside the block: keep what we parsed.
            record(p, ErrorKind::UnclosedBlock);
        }
        TermResult result    = {};
        result.node.kind     = Kind::Block;
        result.node.children = children;
        return result;
    }

    if (b == '"') {
        p->pos += 1;
        usize          chunk_start = p->pos;
        vec::Vec<char> text        = vec::make_vec<char>(p->arena, 0);
        while (p->pos < p->text.len) {
            char c = p->text.data[p->pos];
            if (c == '"') {
                vec::push_all(&text, String{p->pos - chunk_start, p->text.data + chunk_start});
                p->pos += 1;
                // Quoted atoms are always textual, never numbers.
                TermResult result      = {};
                result.node.value.text = vec::slice(text);
                return result;
            }
            bool escape = c == '\\' && p->pos + 1 < p->text.len &&
                          (p->text.data[p->pos + 1] == '"' || p->text.data[p->pos + 1] == '\\');
            if (escape) {
                vec::push_all(&text, String{p->pos - chunk_start, p->text.data + chunk_start});
                vec::push(&text, p->text.data[p->pos + 1]);
                p->pos += 2;
                chunk_start = p->pos;
                continue;
            }
            p->pos += 1;
        }
        return {.error = make_error(p, ErrorKind::UnclosedString)};
    }

    // Bare atom: copy the text into the arena so the tree outlives the source.
    usize start = p->pos;
    while (p->pos < p->text.len && !is_terminator(p->text.data[p->pos])) p->pos += 1;
    String text = arena::clone_string(p->arena, String{p->pos - start, p->text.data + start});

    TermResult result = {};
    result.node.value = value_from_text(text);
    return result;
}

fn OpResult try_op(Parser* p) {
    if (p->pos >= p->text.len) return {};
    char b           = p->text.data[p->pos];
    bool trailing_eq = p->pos + 1 < p->text.len && p->text.data[p->pos + 1] == '=';

    Op    op  = Op::Nil;
    usize len = 0;
    if (b == '=') {
        op  = Op::Eq;
        len = 1;
    } else if (b == '<') {
        op  = trailing_eq ? Op::Le : Op::Lt;
        len = trailing_eq ? 2 : 1;
    } else if (b == '>') {
        op  = trailing_eq ? Op::Ge : Op::Gt;
        len = trailing_eq ? 2 : 1;
    } else if (b == '!' && trailing_eq) {
        op  = Op::Ne;
        len = 2;
    } else if (b == '!') {
        return {.error = make_error(p, ErrorKind::UnexpectedChar)};
    } else {
        return {};
    }
    p->pos += len;
    return {.op = op};
}

// term (op term)? — a keyed pair or a bare value.
fn TermResult parse_item(Parser* p, u32 depth) {
    TermResult first = parse_term(p, depth);
    if (first.error.kind != ErrorKind::Nil) return first;
    if (first.node.kind == Kind::Block) return first;

    skip_trivia(p);
    OpResult op = try_op(p);
    if (op.error.kind != ErrorKind::Nil) return {.error = op.error};
    if (op.op == Op::Nil) return first;

    skip_trivia(p);
    TermResult value = parse_term(p, depth);
    if (value.error.kind != ErrorKind::Nil) return value;
    value.node.key = first.node.value.text;
    value.node.op  = op.op;
    return value;
}

// Parse a whole source file. Never fails; see ParseResult. Everything in the
// result lives in arena.
fn ParseResult parse(arena::Arena* arena, String src) {
    Parser p = {};
    p.text   = src;
    p.arena  = arena;
    p.errors = vec::make_vec<ParseError>(arena, 0);

    ParseResult result = {};
    result.roots       = parse_items(&p, 0);
    result.errors      = vec::slice(p.errors);
    return result;
}

// First child with this key; the permanently-zeroed nil node when there is
// none, so chained lookups are always safe: get(get(root, camp), site).
// Duplicate keys are legal and common — to visit every match, loop children
// and compare keys yourself.
fn const Node* get(const Node* node, String key) {
    for (usize i = 0; i < node->children.len; ++i) {
        const Node* child = &node->children.data[i];
        if (child->key == key) return child;
    }
    return &NIL_NODE;
}

// value of the first child with this key; zero Value when missing or a block.
fn Value get_value(const Node* node, String key) {
    const Node* child = get(node, key);
    if (child->kind != Kind::Atom) return {};
    return child->value;
}

// Text of the first child with this key; empty when missing or a block.
fn String get_text(const Node* node, String key) { return get_value(node, key).text; }

// Number of the first child with this key; 0 when missing or not numeric
// (use get_value().is_number when a real 0 must be told apart).
fn f32 get_number(const Node* node, String key) { return get_value(node, key).number; }

// Indexed access, get's counterpart for array blocks (pos = { 0 0 }): the
// i-th child, the nil node when out of range — indexed reads chain as safely
// as keyed ones. Structural: keyed and bare children count alike.
fn const Node* item(const Node* node, usize index) {
    if (index >= node->children.len) return &NIL_NODE;
    return &node->children.data[index];
}

// Value of the i-th child; zero Value when out of range or a block.
fn Value item_value(const Node* node, usize index) {
    const Node* child = item(node, index);
    if (child->kind != Kind::Atom) return {};
    return child->value;
}

// Text of the i-th child; empty when out of range or a block.
fn String item_text(const Node* node, usize index) { return item_value(node, index).text; }

// Number of the i-th child; 0 when out of range or not numeric
// (use item_value().is_number when a real 0 must be told apart).
fn f32 item_number(const Node* node, usize index) { return item_value(node, index).number; }

// Typed reads with an explicit fallback for when the key is missing or the
// value doesn't fit — for callers whose defaults aren't zero (configs).

// Clausewitz-style booleans: yes/no, or any number (nonzero = true).
fn b32 read_b32(const Node* node, String key, b32 fallback) {
    Value value = get_value(node, key);
    if (value.is_number) return value.number != 0;
    if (value.text == "yes") return true;
    if (value.text == "no") return false;
    return fallback;
}

fn i32 read_i32(const Node* node, String key, i32 fallback) {
    Value value = get_value(node, key);
    if (!value.is_number) return fallback;
    return (i32)value.number;
}

} // namespace tabula
