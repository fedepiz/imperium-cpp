// ROOT: the test suite — dense tripwire tests for core and data modules.
// Tests exist to trip, not to specify: each function exercises many behaviors
// at once, and CHECK's file:line output is what makes a trip diagnosable.
#include "core.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "hashtable.hpp"
#include "list.hpp"
#include "math.hpp"
#include "pool.hpp"
#include "string.hpp"
#include "vec.hpp"
#include "tabula.hpp"
#include "ui/layout.hpp"
#include "ui/style.hpp"
#include "ui/ir.hpp"
#include "ui/ui.hpp"


#include <stdio.h>

// Failing a CHECK fails this test (early return) but not the suite.
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("\n  FAIL %s:%d: CHECK(%s)", __FILE__, __LINE__, #cond);                                            \
            return false;                                                                                              \
        }                                                                                                              \
    } while (0)

namespace {

fn b32 test_arena() {
    arena::Arena a = {};
    arena::reserve(&a, 100 * 1024); // rounds up to a commit-chunk multiple
    CHECK(a.base != 0 && a.size >= 100 * 1024 && a.size % (64 * 1024) == 0 && a.used == 0);

    u8* p = arena::allocate_raw(&a, 100, 16);
    CHECK(p == a.base && ((usize)p & 15) == 0);
    u8* q = arena::allocate_raw(&a, 100 * 1024, 8); // grows committed past the first chunk
    CHECK(q != 0 && q >= p + 100);
    q[100 * 1024 - 1] = 42;                         // touching the far end must not fault
    CHECK(arena::allocate_raw(&a, a.size, 1) == 0); // doesn't fit -> ZII null

    usize used = a.used;
    {
        arena::ScratchArena scratch(&a);
        arena::allocate<u64>(&a, 1000);
        CHECK(a.used > used);
    }
    CHECK(a.used == used); // watermark restored at scope exit

    // reset rewinds but keeps pages committed; reused memory comes back zeroed
    memset(a.base, 0xAB, a.used);
    arena::reset(&a);
    CHECK(a.used == 0);
    u8* r = arena::allocate_raw(&a, 256, 1);
    CHECK(r == a.base);
    for (usize i = 0; i < 256; ++i) CHECK(r[i] == 0);

    String s = arena::clone_string(&a, "Roma");
    String t = arena::clone_slice(&a, s);
    CHECK(t.len == 4 && string::equals(s, t) && t.data != s.data);

    arena::Arena dead = {};
    CHECK(arena::allocate<u32>(&dead, 4) == 0); // ZII: zero arena allocates null, no crash
    arena::release(&a);
    CHECK(a.base == 0 && a.size == 0 && a.used == 0);
    return true;
}

fn b32 test_vec() {
    arena::Arena a = {};
    arena::reserve(&a, 4 * 1024 * 1024);

    vec::Vec<i32> zero = {};
    for (i32 v : zero) { (void)v; CHECK(false); } // ZII vec iterates nothing
    vec::clear(&zero);

    vec::Vec<i32> v = vec::make_vec<i32>(&a, 4);
    for (i32 i = 0; i < 1000; ++i) vec::push(&v, i);
    CHECK(v.len == 1000 && v.capacity >= 1000);
    CHECK((u8*)v.data == a.base); // sole tail allocation: growth stayed in place
    vec::pop(&v);
    vec::pop(&v);
    CHECK(v.len == 998 && v[997] == 997 && v[0] == 0);

    (void)arena::allocate<u64>(&a); // block the tail: the next growth must relocate
    u8* before = (u8*)v.data;
    while ((u8*)v.data == before) vec::push(&v, 7);
    b32 intact = true;
    for (usize i = 0; i < 998; ++i) intact = intact && v[i] == (i32)i;
    CHECK(intact && v[v.len - 1] == 7); // relocation preserved contents

    i32           backing[3] = {5, 6, 7};
    vec::Vec<i32> w          = vec::make_vec(&a, Slice<i32>{3, backing});
    Slice<i32>    ws         = vec::slice(&w);
    CHECK(w.len == 3 && ws[2] == 7 && ws.data != backing); // made from a slice = a copy
    vec::push_all(&w, Slice<i32>{3, backing});
    CHECK(w.len == 6 && w[3] == 5 && w[5] == 7);

    struct Pair { u64 k, v; };
    vec::Vec<Pair> pairs = vec::make_vec<Pair>(&a, 0);
    for (u64 i = 0; i < 100; ++i) vec::push(&pairs, {i, i * i});
    CHECK(pairs[99].v == 99 * 99 && ((usize)pairs.data % alignof(Pair)) == 0);

    arena::Arena  dead   = {};
    vec::Vec<i32> broken = vec::make_vec<i32>(&dead, 16);
    CHECK(broken.len == 0 && broken.capacity == 0 && broken.data == 0); // ZII soft-fail

    arena::release(&a);
    return true;
}

fn b32 test_list() {
    arena::Arena a = {};
    arena::reserve(&a, 4 * 1024 * 1024);

    list::List<i32> zero = {};
    for (i32 v : zero) { (void)v; CHECK(false); } // ZII list iterates nothing
    CHECK(list::front(&zero) == 0 && list::back(&zero) == 0 && zero.len == 0);
    list::clear(&zero);

    // Collector mode: pushes never relocate — pointers taken early stay valid
    // across chunk growth; chunks double from 64.
    list::List<i32> l     = list::make_list<i32>(&a);
    i32*            fifth = 0;
    for (i32 i = 0; i < 1000; ++i) {
        i32* slot = list::push(&l, i);
        if (i == 5) fifth = slot;
        CHECK(*slot == i && slot == list::back(&l));
    }
    CHECK(l.len == 1000 && *fifth == 5 && fifth == &l.first->values[5]);
    CHECK(l.first->cap == 64 && l.first->next->cap == 128); // doubling chain
    i32 expected = 0;
    for (i32 v : l) { CHECK(v == expected); expected += 1; } // in-order, chunk-hopping
    CHECK(expected == 1000);

    // Stack mode: pop_back trims and unlinks emptied tail chunks.
    for (i32 i = 999; i >= 64; --i) {
        CHECK(*list::back(&l) == i);
        list::pop_back(&l);
    }
    CHECK(l.len == 64 && l.first == l.last && *list::back(&l) == 63);

    // Queue mode: FIFO drain via front/pop_front, interleaved with pushes
    // that straddle the chunk boundary.
    for (i32 i = 0; i < 32; ++i) list::push(&l, 1000 + i); // 96 total: spills into a new chunk
    CHECK(l.first != l.last);
    for (i32 i = 0; i < 64; ++i) {
        CHECK(*list::front(&l) == i);
        list::pop_front(&l);
    }
    CHECK(l.len == 32 && l.first == l.last && l.head == 0); // spent chunk abandoned
    i32 seen = 0;
    while (i32* item = list::front(&l)) {
        CHECK(*item == 1000 + seen);
        list::pop_front(&l);
        seen += 1;
    }
    CHECK(seen == 32 && l.len == 0 && l.first == 0 && l.last == 0 && l.head == 0); // back to zero state

    // Emptying from both ends meets in the middle and re-zeroes cleanly.
    for (i32 i = 0; i < 101; ++i) list::push(&l, i); // odd count: a middle element exists
    while (l.len > 1) {
        list::pop_front(&l);
        list::pop_back(&l);
    }
    CHECK(*list::front(&l) == 50 && list::front(&l) == list::back(&l));
    list::pop_back(&l);
    CHECK(l.len == 0 && l.first == 0 && list::push(&l, 7) != 0 && *list::back(&l) == 7); // reusable after empty

    struct Fat { u64 pad[4]; u64 id; };
    list::List<Fat> fat = list::make_list<Fat>(&a);
    Fat*            f0  = list::push(&fat, Fat{{}, 1});
    for (u64 i = 2; i <= 200; ++i) list::push(&fat, Fat{{}, i});
    CHECK(f0->id == 1 && list::back(&fat)->id == 200 && fat.len == 200); // fat elements, stable too

    arena::release(&a);
    return true;
}

fn b32 test_string() {
    CHECK(string::equals(String{}, ""));
    CHECK(string::equals("Roma", "Roma"));
    CHECK(!string::equals("Roma", "Rom") && !string::equals("Roma", "Ostia"));
    CHECK(String((const char*)0).len == 0); // ZII: null views as empty

    struct Case { const char* text; f32 value; b32 ok; };
    Case cases[] = {
        {"4800", 4800.0f, true},  {"-0.5", -0.5f, true},     {"4.8e3", 4800.0f, true},
        {"1e-2", 0.01f, true},    {"+5", 5.0f, true},        {"5.", 5.0f, true},
        {".5", 0.5f, true},       {"", 0, false},            {"brutus", 0, false},
        {"1444.11.11", 0, false}, {"1e", 0, false},          {"inf", 0, false},
        {"--1", 0, false},        {"1 ", 0, false},
    };
    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        string::ParseF32Result r = string::parse_f32(cases[i].text);
        CHECK(r.ok == cases[i].ok && r.value == cases[i].value);
    }
    return true;
}

fn b32 test_file_io() {
    arena::Arena a = {};
    arena::reserve(&a, 1024 * 1024);

    // Binary round-trip: write a fixture with plain stdio (zeros and high
    // bytes included), read it back through the module. Paths are relative to
    // the repo root — run via ./x.sh test.
    const char* path = "build/file_io_fixture.tmp";
    u8          payload[300];
    for (usize i = 0; i < sizeof(payload); ++i) payload[i] = (u8)(i * 7);
    FILE* f = fopen(path, "wb");
    CHECK(f != 0 && fwrite(payload, 1, sizeof(payload), f) == sizeof(payload));
    fclose(f);

    file_io::ReadFile<Slice<u8>> bytes = file_io::read_file_to_bytes(&a, path);
    CHECK(bytes.messages.len == 0 && bytes.data.len == sizeof(payload));
    CHECK(memcmp(bytes.data.data, payload, sizeof(payload)) == 0);

    file_io::ReadFile<String> text = file_io::read_file_to_string(&a, path);
    CHECK(text.messages.len == 0 && text.data.len == sizeof(payload));
    CHECK((u8)text.data[0] == 0 && (u8)text.data[299] == (u8)(299 * 7)); // binary-safe, embedded NUL survives

    // Truncated to empty: a successful read of nothing (ZII, not an error).
    f = fopen(path, "wb");
    CHECK(f != 0);
    fclose(f);
    file_io::ReadFile<Slice<u8>> empty = file_io::read_file_to_bytes(&a, path);
    CHECK(empty.messages.len == 0 && empty.data.len == 0 && empty.data.data == 0);
    remove(path);

    // Missing file: zero data plus one message naming the path and cause.
    file_io::ReadFile<Slice<u8>> missing = file_io::read_file_to_bytes(&a, "build/does_not_exist.tmp");
    CHECK(missing.data.len == 0 && missing.data.data == 0 && missing.messages.len == 1);
    file_io::Error* error = list::front(&missing.messages);
    CHECK(error->kind == file_io::ErrorKind::NotFound);
    CHECK(error->message.len > 24 && memcmp(error->message.data, "build/does_not_exist.tmp", 24) == 0);

    // A directory is not a readable file (which step fails is OS-dependent).
    file_io::ReadFile<Slice<u8>> dir = file_io::read_file_to_bytes(&a, "build");
    CHECK(dir.messages.len == 1 && dir.data.len == 0);

    arena::release(&a);
    return true;
}

fn b32 test_tabula() {
    arena::Arena a = {};
    arena::reserve(&a, 1024 * 1024);

    // One dense source: comments, escapes, numbers vs dates vs quoted numbers,
    // every operator, arrays, nesting, duplicate keys, quoted keys, anonymous
    // blocks, UTF-8.
    const char* src =
        "# roster\n"
        "legion = {\n"
        "    name = \"Legio \\\"I\\\" Italica\"\n"
        "    strength = 4800\n"
        "    morale > 0.5\n"
        "    founded = 42.3.1\n"
        "    id = \"12\"\n"
        "    cohorts = { 1 2 3 }\n"
        "    camp = { site = roma }\n"
        "}\n"
        "legion = { name = \"Legio II\" }\n"
        "\"anno urbis\" <= -5e-1\n"
        "x != 1 y >= 2 z < 3\n"
        "{ 皇帝 = 帝国 }\n";

    // Parse from a buffer we clobber afterwards: the tree must only reference
    // the arena, never the source.
    char  buffer[512];
    usize n = strlen(src);
    CHECK(n < sizeof(buffer));
    memcpy(buffer, src, n);
    tabula::ParseResult r = tabula::parse(&a, String{n, buffer});
    memset(buffer, 0xDD, sizeof(buffer));

    CHECK(r.errors.len == 0 && r.roots.len == 7);

    const tabula::Node* legion = &r.roots[0];
    CHECK(string::equals(legion->key, "legion") && legion->op == tabula::Op::Eq && legion->kind == tabula::Kind::Block);
    CHECK(string::equals(tabula::get_text(legion, "name"), "Legio \"I\" Italica")); // escapes resolved
    CHECK(tabula::get_number(legion, "strength") == 4800.0f);
    CHECK(tabula::get(legion, "morale")->op == tabula::Op::Gt);
    tabula::Value founded = tabula::get_value(legion, "founded");
    CHECK(!founded.is_number && string::equals(founded.text, "42.3.1")); // dates stay text
    CHECK(!tabula::get_value(legion, "id").is_number);                   // quoted "12" stays text
    CHECK(tabula::get_number(legion, "name") == 0.0f);                   // text atom is not a number

    const tabula::Node* cohorts = tabula::get(legion, "cohorts");
    CHECK(cohorts->children.len == 3 && cohorts->children[2].value.number == 3.0f);
    CHECK(cohorts->children[0].key.len == 0 && cohorts->children[0].op == tabula::Op::Nil);
    CHECK(string::equals(tabula::get_text(tabula::get(legion, "camp"), "site"), "roma")); // chained get

    usize legions = 0; // duplicate keys: visiting all matches is a plain loop
    for (usize i = 0; i < r.roots.len; ++i) {
        if (string::equals(r.roots[i].key, "legion")) legions += 1;
    }
    CHECK(legions == 2 && string::equals(tabula::get_text(&r.roots[1], "name"), "Legio II"));

    CHECK(string::equals(r.roots[2].key, "anno urbis") && r.roots[2].op == tabula::Op::Le &&
          r.roots[2].value.number == -0.5f);
    CHECK(r.roots[3].op == tabula::Op::Ne && r.roots[4].op == tabula::Op::Ge && r.roots[5].op == tabula::Op::Lt);
    const tabula::Node* block = &r.roots[6];
    CHECK(block->key.len == 0 && block->kind == tabula::Kind::Block);
    CHECK(string::equals(block->children[0].key, "皇帝") && string::equals(block->children[0].value.text, "帝国"));

    // Missing keys: nil node chains safely and reads as zero.
    const tabula::Node* nil = tabula::get(tabula::get(legion, "missing"), "worse");
    CHECK(nil->kind == tabula::Kind::Atom && nil->children.len == 0 && nil->value.text.len == 0);
    CHECK(tabula::get_number(legion, "missing") == 0.0f && tabula::get_text(legion, "missing").len == 0);

    tabula::Node        zn = {};
    tabula::ParseResult zr = {};
    CHECK(zn.op == tabula::Op::Nil && zn.kind == tabula::Kind::Atom && zr.roots.len == 0 && zr.errors.len == 0);

    // Fallback reads: yes/no and numeric booleans, unrecognized text trips
    // the fallback, i32 reads reject non-numbers.
    r = tabula::parse(&a, "flag = yes off = no n = 42 s = word");
    tabula::Node root = {};
    root.kind         = tabula::Kind::Block;
    root.children     = r.roots;
    CHECK(tabula::read_b32(&root, "flag", false) && !tabula::read_b32(&root, "off", true));
    CHECK(tabula::read_b32(&root, "n", false) && tabula::read_b32(&root, "missing", true));
    CHECK(!tabula::read_b32(&root, "s", false)); // unrecognized text -> fallback
    CHECK(tabula::read_i32(&root, "n", 0) == 42 && tabula::read_i32(&root, "s", 7) == 7);
    CHECK(tabula::read_i32(&root, "missing", 9) == 9);

    arena::release(&a);
    return true;
}

fn b32 test_tabula_errors() {
    arena::Arena a = {};
    arena::reserve(&a, 1024 * 1024);

    struct Case { const char* src; tabula::ErrorKind kind; };
    Case cases[] = {
        {"a = { b = 1", tabula::ErrorKind::UnclosedBlock},
        {"a = \"oops", tabula::ErrorKind::UnclosedString},
        {"} b = 1", tabula::ErrorKind::UnexpectedCloseBrace},
        {"a = }", tabula::ErrorKind::ExpectedValue},
        {"a =", tabula::ErrorKind::ExpectedValue},
        {"= 1", tabula::ErrorKind::UnexpectedChar},
        {"a ! b", tabula::ErrorKind::UnexpectedChar},
    };
    for (usize i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        tabula::ParseResult r = tabula::parse(&a, cases[i].src);
        CHECK(r.errors.len > 0 && r.errors[0].kind == cases[i].kind);
    }

    // Recovery: positions are exact, good items survive around the bad ones.
    tabula::ParseResult r = tabula::parse(&a, "a = 1\n= oops\nb = \"unclosed");
    CHECK(r.errors.len == 2);
    CHECK(r.errors[0].kind == tabula::ErrorKind::UnexpectedChar && r.errors[0].line == 2 && r.errors[0].col == 1);
    CHECK(string::equals(r.errors[0].message, "2:1: unexpected character"));
    CHECK(r.errors[1].kind == tabula::ErrorKind::UnclosedString && r.errors[1].line == 3 && r.errors[1].col == 14 &&
          r.errors[1].offset == 26);
    CHECK(string::equals(r.errors[1].message, "3:14: unclosed string"));
    CHECK(r.roots.len == 2 && string::equals(r.roots[0].key, "a") && string::equals(r.roots[1].value.text, "oops"));

    r = tabula::parse(&a, "a = { b = 1"); // EOF inside a block keeps the partial tree
    CHECK(r.errors.len == 1 && r.errors[0].offset == 11 && r.errors[0].col == 12);
    CHECK(string::equals(tabula::get_text(&r.roots[0], "b"), "1"));

    const char* junk[] = {"}}}}", "= = = =", "{{{", "a = = 1", "!!!", "\"", "{ } }"};
    for (usize i = 0; i < sizeof(junk) / sizeof(junk[0]); ++i) {
        CHECK(tabula::parse(&a, junk[i]).errors.len > 0); // errors, never a hang or crash
    }

    char deep[600];
    memset(deep, '{', sizeof(deep));
    r = tabula::parse(&a, String{sizeof(deep), deep});
    CHECK(r.errors.len > 0 && r.errors[0].kind == tabula::ErrorKind::TooDeep);

    arena::release(&a);
    return true;
}

struct TestKey {
    u64 value;
};

// Behavioral only — the key is an opaque u64, and the test never interprets
// its bits: it checks the properties the pool promises, not how they're kept.
fn b32 test_pool() {
    pool::Pool<TestKey, u64, 4> p = {}; // slot 0 is the dummy; 3 usable
    TestKey null_key = {};
    CHECK(p.live_count == 0 && p[null_key] == 0); // ZII: a miss reads valid empty data
    for (auto& entry : p) {
        (void)entry;
        CHECK(false); // zero pool iterates nothing
    }

    TestKey a = pool::alloc(&p);
    TestKey b = pool::alloc(&p);
    TestKey c = pool::alloc(&p);
    CHECK(!pool::is_nil_key(a) && !pool::is_nil_key(b) && !pool::is_nil_key(c) && p.live_count == 3);
    CHECK(a.value != b.value && b.value != c.value && a.value != c.value); // distinct handles
    CHECK(pool::is_nil_key(null_key) && p[a] == 0);                       // fresh slots come back zeroed
    CHECK(pool::is_nil_key(pool::alloc(&p)));                             // full -> nil key, no trap

    p[b] = 42;
    p[c] = 5;
    CHECK(p[b] == 42 && p[c] == 5 && p[a] == 0); // entries are independent

    CHECK(pool::free(&p, b) && !pool::free(&p, b));                      // b32 result: freed once, stale after
    CHECK(!pool::free(&p, null_key) && !pool::free(&p, TestKey{~0ull})); // nil and garbage keys are no-ops
    CHECK(p[b] == 0 && p.live_count == 2); // the stale key misses — reads empty, not the old 42

    TestKey b2 = pool::alloc(&p); // freed capacity comes back
    CHECK(!pool::is_nil_key(b2) && b2.value != b.value && p[b2] == 0); // fresh handle, zeroed on reuse
    p[b2] = 7;
    CHECK(p[b] == 0 && p[b2] == 7); // old key still misses — no aliasing with its slot's new tenant

    pool::free(&p, a);
    u64 sum  = 0;
    u32 seen = 0;
    for (auto& entry : p) { // live entries only: freed ones skipped
        CHECK(!pool::is_nil_key(entry.key) && &p[entry.key] == &entry.value); // iterated keys look themselves up
        sum += entry.value;
        seen += 1;
    }
    CHECK(seen == 2 && sum == 12); // b2 (7) + c (5)

    TestKey d = pool::insert(&p, (u64)30); // alloc + assign in one step
    CHECK(!pool::is_nil_key(d) && p[d] == 30 && p.live_count == 3);
    CHECK(pool::is_nil_key(pool::insert(&p, (u64)31))); // full again -> nil key
    CHECK(p[null_key] == 0 && p[b] == 0);               // misses still read clean after the churn

    return true;
}

fn b32 test_math() {
    math::Rect r = {10, 20, 30, 40};
    CHECK(math::contains(r, {10, 20}) && math::contains(r, {39.9f, 59.9f}));    // corners: min in
    CHECK(!math::contains(r, {40, 30}) && !math::contains(r, {20, 60}));        // ...max out (half-open)
    CHECK(!math::contains(r, {9.9f, 20}) && !math::contains({}, {}));           // ZII rect contains nothing
    math::V2 a = {1, 2};
    CHECK((a == math::V2{1, 2} && !(a == math::V2{2, 1})));                      // defaulted comparisons
    CHECK((math::V2{1, 2} < math::V2{1, 3} && !(math::Rect{} != math::Rect{}))); // <=> lexicographic order
    return true;
}

// The field can't be named `fn` — that's the function keyword (a macro).
fn b32 test_hashtable() {
    arena::Arena a = {};
    arena::reserve(&a, 4 * 1024 * 1024);

    hashtable::Hashtable<i32> zero = {};
    CHECK(hashtable::get(&zero, 42) == 0); // ZII: reads on the zero table are safe
    hashtable::clear(&zero);
    for (auto entry : zero) { (void)entry; CHECK(false); } // and it iterates nothing

    hashtable::Hashtable<i32> t = hashtable::make_table<i32>(&a, 4);
    for (u64 k = 1; k <= 500; ++k) *hashtable::put(&t, k) = (i32)k * 3; // grows through many doublings
    CHECK(t.count == 500);
    b32 intact = true;
    for (u64 k = 1; k <= 500; ++k) intact = intact && hashtable::get(&t, k) && *hashtable::get(&t, k) == (i32)k * 3;
    CHECK(intact && hashtable::get(&t, 501) == 0);

    *hashtable::put(&t, 7) = -1; // put on a present key returns the same slot
    CHECK(t.count == 500 && *hashtable::get(&t, 7) == -1);

    for (u64 k = 2; k <= 500; k += 2) CHECK(hashtable::remove(&t, k));
    CHECK(!hashtable::remove(&t, 2) && t.count == 250); // double-remove is a no-op
    intact = true; // backward-shift deletion must not orphan any surviving probe chain
    for (u64 k = 1; k <= 500; k += 2)
        intact = intact && hashtable::get(&t, k) && *hashtable::get(&t, k) == (k == 7 ? -1 : (i32)k * 3);
    for (u64 k = 2; k <= 500; k += 2) intact = intact && hashtable::get(&t, k) == 0;
    CHECK(intact);

    usize seen = 0;
    for (auto entry : t) { seen += 1; intact = intact && *entry.value == (entry.key == 7 ? -1 : (i32)entry.key * 3); }
    CHECK(seen == 250 && intact);

    hashtable::clear(&t);
    CHECK(t.count == 0 && hashtable::get(&t, 1) == 0);
    *hashtable::put(&t, 1) = 5; // reusable after clear
    CHECK(t.count == 1 && *hashtable::get(&t, 1) == 5);

    arena::release(&a);
    return true;
}

// ---------------------------------------------------------------- ui layout
// Ported from the Rust ui crate's layout.rs test suite — the behavioral spec
// of the engine. Condensed into dense tripwires, grouped by topic.

fn ui::layout::Input ui_input(f32 w, f32 h) {
    ui::layout::Input in = {};
    in.bounds = {0, 0, w, h};
    return in;
}

// First command carrying the id (any kind), like the Rust tests' command().
fn const ui::layout::DrawCommand* ui_find(ui::layout::Output* out, ui::layout::ElementId id) {
    for (const ui::layout::DrawCommand& command : out->commands) {
        if (command.id == id) return &command;
    }
    return 0;
}

fn const ui::layout::DrawCommand* ui_find_kind(ui::layout::Output* out, ui::layout::ElementId id,
                                               ui::layout::DrawKind kind) {
    for (const ui::layout::DrawCommand& command : out->commands) {
        if (command.id == id && command.kind == kind) return &command;
    }
    return 0;
}

fn b32 approx(f32 a, f32 b) { return fabsf(a - b) < 0.001f; }

fn b32 test_ui_grow() {
    namespace L = ui::layout;
    auto zero = [](String, u16) { return L::TextMetrics{}; };

    // Grow siblings split what a fixed sibling leaves; weights scale shares;
    // caps redistribute to the others; fractional caps resolve on the parent.
    L::Engine  engine = {};
    L::Output* out    = L::layout(&engine, ui_input(300, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.id = "fixed", .width = L::px(100), .height = L::grow(), .background = math::RED});
            L::add(u, {.id = "grow-a", .width = L::grow(), .height = L::grow(), .background = math::GREEN});
            L::add(u, {.id = "grow-b", .width = L::grow(), .height = L::grow(), .background = math::BLUE});
        });
    });
    CHECK(ui_find(out, "fixed")->bounds.w == 100);
    CHECK(ui_find(out, "grow-a")->bounds.w == 100);
    CHECK(ui_find(out, "grow-b")->bounds.w == 100 && ui_find(out, "grow-b")->bounds.x == 200);

    L::Engine weighted = {};
    out = L::layout(&weighted, ui_input(300, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.id = "one", .width = L::grow(), .height = L::grow(), .background = math::RED});
            L::add(u, {.id = "two", .width = L::grow(2), .height = L::grow(), .background = math::BLUE});
        });
    });
    CHECK(approx(ui_find(out, "one")->bounds.w, 100) && approx(ui_find(out, "two")->bounds.w, 200));

    L::Engine capped = {};
    out = L::layout(&capped, ui_input(300, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.id = "capped", .width = L::grow(), .max_width = 50, .height = L::grow(),
                       .background = math::RED});
            L::add(u, {.id = "remainder", .width = L::grow(2), .height = L::grow(), .background = math::BLUE});
        });
    });
    CHECK(ui_find(out, "capped")->bounds.w == 50 && approx(ui_find(out, "remainder")->bounds.w, 250));

    L::Engine fractional = {};
    out = L::layout(&fractional, ui_input(400, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.id = "half", .width = L::grow(), .max_fraction = {0.5f, 0}, .height = L::grow(),
                       .background = math::RED});
            L::add(u, {.id = "rest", .width = L::grow(), .height = L::grow(), .background = math::BLUE});
        });
    });
    CHECK(ui_find(out, "half")->bounds.w == 200 && ui_find(out, "rest")->bounds.w == 200);
    return true;
}

fn b32 test_ui_compress() {
    namespace L = ui::layout;
    auto zero = [](String, u16) { return L::TextMetrics{}; };

    // Overflowing fixed children compress proportionally, stop at minimums,
    // and reserve growing siblings' floors; growing text never shrinks below
    // its measure.
    L::Engine  engine = {};
    L::Output* out    = L::layout(&engine, ui_input(150, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.id = "a", .width = L::px(100), .height = L::grow(), .background = math::RED});
            L::add(u, {.id = "b", .width = L::px(100), .height = L::grow(), .background = math::BLUE});
        });
    });
    CHECK(approx(ui_find(out, "a")->bounds.w, 75) && approx(ui_find(out, "b")->bounds.w, 75));
    CHECK(approx(ui_find(out, "b")->bounds.x, 75));

    L::Engine minimums = {};
    out = L::layout(&minimums, ui_input(150, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.id = "minimum", .width = L::px(100), .min_width = 90, .height = L::grow(),
                       .background = math::RED});
            L::add(u, {.id = "compressible", .width = L::px(100), .height = L::grow(), .background = math::BLUE});
        });
    });
    CHECK(ui_find(out, "minimum")->bounds.w == 90 && approx(ui_find(out, "compressible")->bounds.w, 60));

    L::Engine reserve = {};
    out = L::layout(&reserve, ui_input(200, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.id = "fixed", .width = L::px(200), .height = L::grow(), .background = math::RED});
            L::add(u, {.id = "grow", .width = L::grow(), .min_width = 50, .height = L::grow(),
                       .background = math::BLUE});
        });
    });
    CHECK(approx(ui_find(out, "fixed")->bounds.w, 150) && ui_find(out, "grow")->bounds.w == 50);

    L::Engine text_floor = {};
    auto      measure    = [](String text, u16) { return L::TextMetrics{{(f32)text.len * 10, 10}, 10}; };
    out = L::layout(&text_floor, ui_input(200, 100), measure, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            // 15 chars = 150 px of text: the greedy sibling must not squeeze
            // it below its measure.
            L::add(u, {.id = "text", .width = L::grow(), .height = L::grow(), .text = {.text = "fifteen chars!!"}});
            L::add(u, {.id = "greedy", .width = L::grow(100), .height = L::grow(), .background = math::BLUE});
        });
    });
    CHECK(ui_find(out, "text")->bounds.w == 150 && approx(ui_find(out, "greedy")->bounds.w, 50));
    return true;
}

fn b32 test_ui_fit() {
    namespace L = ui::layout;
    auto zero = [](String, u16) { return L::TextMetrics{}; };

    // A fit parent must allocate a grow-to-120 child its cap, like a fixed
    // size — it intends to grow into it.
    L::Engine  engine = {};
    L::Output* out    = L::layout(&engine, ui_input(400, 400), zero, [&](L::Ui* u) {
        L::add_with(u, {.id = "fit", .background = math::BLUE}, [&] {
            L::add(u, {.id = "capped", .width = L::grow(), .max_width = 120, .height = L::px(10),
                       .background = math::RED});
        });
    });
    CHECK(ui_find(out, "fit")->bounds.w == 120 && ui_find(out, "capped")->bounds.w == 120);

    L::Engine minmax  = {};
    auto      measure = [](String text, u16) { return L::TextMetrics{{(f32)text.len * 10, 10}, 10}; };
    out = L::layout(&minmax, ui_input(300, 100), measure, [&](L::Ui* u) {
        L::add(u, {.id = "maximum", .max_width = 40, .text = {.text = "wide text"}, .background = math::RED});
        L::add(u, {.id = "minimum", .min_width = 30, .height = L::px(10), .background = math::BLUE});
    });
    CHECK(ui_find(out, "maximum")->bounds.w == 40 && ui_find(out, "minimum")->bounds.w == 30);

    L::Engine padded       = {};
    auto      size_measure = [](String text, u16 size) {
        return L::TextMetrics{{(f32)text.len * (f32)size, (f32)size}, (f32)size};
    };
    out = L::layout(&padded, ui_input(400, 200), size_measure, [&](L::Ui* u) {
        L::add(u, {.id = "text", .padding = L::pad_all(5),
                   .text = {.text = "abc", .size = 10, .color = math::RED}, .background = math::BLUE});
    });
    CHECK(ui_find(out, "text")->bounds.w == 40 && ui_find(out, "text")->bounds.h == 20);
    {
        const L::DrawCommand* text = ui_find_kind(out, "text", L::DrawKind::Text);
        CHECK((text && text->bounds == math::Rect{5, 5, 30, 10}));
        CHECK(string::equals(text->text, "abc"));
    }

    L::Engine vertical = {};
    out = L::layout(&vertical, ui_input(200, 200), zero, [&](L::Ui* u) {
        L::add_with(u,
                    {.width = L::grow(), .height = L::grow(), .direction = L::Direction::TopToBottom,
                     .padding = L::pad_all(10), .gap = 5, .align_x = L::Align::Center, .align_y = L::Align::End},
                    [&] {
                        L::add(u, {.id = "a", .width = L::px(20), .height = L::px(30), .background = math::RED});
                        L::add(u, {.id = "b", .width = L::px(20), .height = L::px(30), .background = math::GREEN});
                    });
    });
    CHECK(ui_find(out, "a")->bounds.x == 90 && ui_find(out, "a")->bounds.y == 125);
    CHECK(ui_find(out, "b")->bounds.y == 160);

    // Parent fractions resolve against the parent's inner (padded) size.
    L::Engine fraction = {};
    out = L::layout(&fraction, ui_input(200, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow(), .padding = L::pad_symmetric(10, 0)}, [&] {
            L::add(u, {.id = "half", .width = L::frac(0.5f), .height = L::grow(), .background = math::RED});
        });
    });
    CHECK(ui_find(out, "half")->bounds.x == 10 && ui_find(out, "half")->bounds.w == 90);
    return true;
}

fn b32 test_ui_text() {
    namespace L = ui::layout;
    auto measure = [](String text, u16) { return L::TextMetrics{{(f32)text.len * 10, 10}, 10}; };

    // Wraps at words; the second pass updates the layout height.
    L::Engine  engine = {};
    L::Output* out    = L::layout(&engine, ui_input(100, 100), measure, [&](L::Ui* u) {
        L::add(u, {.id = "wrapped", .width = L::px(35), .text = {.text = "one two", .size = 10, .wrap = true},
                   .background = math::RED});
    });
    CHECK(ui_find_kind(out, "wrapped", L::DrawKind::Rectangle)->bounds.h == 20);
    {
        usize                 lines = 0;
        const L::DrawCommand* line_commands[4] = {};
        for (const L::DrawCommand& command : out->commands) {
            if (command.id == L::ElementId("wrapped") && command.kind == L::DrawKind::Text && lines < 4) {
                line_commands[lines] = &command;
                lines += 1;
            }
        }
        CHECK(lines == 2);
        CHECK(string::equals(line_commands[0]->text, "one"));
        CHECK(string::equals(line_commands[1]->text, "two"));
        CHECK(line_commands[1]->bounds.y == 10);
    }

    // Long words hard-break; explicit newlines are preserved.
    L::Engine breaks = {};
    out = L::layout(&breaks, ui_input(100, 100), measure, [&](L::Ui* u) {
        L::add(u, {.id = "wrapped", .width = L::px(25), .text = {.text = "abcd\nef", .wrap = true}});
    });
    CHECK(out->commands.len == 3);
    CHECK(string::equals(out->commands[0].text, "ab"));
    CHECK(string::equals(out->commands[1].text, "cd"));
    CHECK(string::equals(out->commands[2].text, "ef"));
    CHECK(out->commands[2].bounds.y == 20);

    // Generational cache: a hit every frame keeps entries alive; a frame
    // without the text evicts them — except the per-size line probe.
    L::Engine cached = {};
    i32       calls  = 0;
    auto      counting = [&](String text, u16) {
        calls += 1;
        return L::TextMetrics{{(f32)text.len * 10, 10}, 10};
    };
    auto build = [](L::Ui* u) {
        L::add(u, {.width = L::px(60), .text = {.text = "cached text", .size = 12, .wrap = true}});
    };
    (void)L::layout(&cached, ui_input(100, 100), counting, build);
    i32 first_frame_calls = calls;
    CHECK(first_frame_calls > 0);
    (void)L::layout(&cached, ui_input(100, 100), counting, build);
    CHECK(calls == first_frame_calls);
    (void)L::layout(&cached, ui_input(100, 100), counting, [](L::Ui*) {});
    (void)L::layout(&cached, ui_input(100, 100), counting, build);
    CHECK(calls == first_frame_calls * 2 - 1);

    // The measured baseline rides through to the draw command.
    L::Engine baseline = {};
    out = L::layout(&baseline, ui_input(100, 100), [](String, u16) { return L::TextMetrics{{20, 12}, 9}; },
                    [&](L::Ui* u) { L::add(u, {.id = "text", .text = {.text = "gyp", .size = 12}}); });
    CHECK(ui_find(out, "text")->text_baseline == 9);

    // Negative padding is treated as zero (clamped by max with 0).
    L::Engine negative = {};
    out = L::layout(&negative, ui_input(100, 100), [](String, u16) { return L::TextMetrics{{5, 5}, 5}; },
                    [&](L::Ui* u) {
                        L::add(u, {.id = "text", .padding = L::pad_all(-10), .text = {.text = "x", .size = 5},
                                   .background = math::RED});
                    });
    CHECK(ui_find(out, "text")->bounds.w == 5 && ui_find(out, "text")->bounds.h == 5);
    return true;
}

fn b32 test_ui_sense() {
    namespace L = ui::layout;
    auto zero = [](String, u16) { return L::TextMetrics{}; };

    // Sense is judged against last frame's bounds: nothing on the first
    // frame, everything on the second with the pointer down over the button.
    L::Engine engine = {};
    auto      button = [](L::Ui* u) {
        return L::add(u, {.id = "button", .width = L::px(100), .height = L::px(40), .background = math::RED});
    };
    L::Sense first = {};
    (void)L::layout(&engine, ui_input(200, 100), zero, [&](L::Ui* u) { first = button(u); });
    CHECK(!first.clicked && !first.hovered && !first.held);

    L::Input second_input   = ui_input(200, 100);
    second_input.mouse_pos     = {50, 20};
    second_input.mouse_pressed = true;
    second_input.mouse_down    = true;
    L::Sense second = {};
    (void)L::layout(&engine, second_input, zero, [&](L::Ui* u) { second = button(u); });
    CHECK(second.clicked && second.hovered && second.held);

    // Anonymous elements get structural ids, stable across matching frames.
    L::Engine anonymous = {};
    (void)L::layout(&anonymous, ui_input(100, 100), zero,
                    [&](L::Ui* u) { L::add(u, {.width = L::px(40), .height = L::px(30)}); });
    L::Input probe  = ui_input(100, 100);
    probe.mouse_pos = {20, 15};
    L::Sense sensed = {};
    (void)L::layout(&anonymous, probe, zero,
                    [&](L::Ui* u) { sensed = L::add(u, {.width = L::px(40), .height = L::px(30)}); });
    CHECK(sensed.hovered);

    // Sense is queryable before the element is declared and agrees with the
    // Sense returned by add.
    L::Engine early = {};
    (void)L::layout(&early, ui_input(200, 100), zero, [&](L::Ui* u) { (void)button(u); });
    L::Input press        = ui_input(200, 100);
    press.mouse_pos       = {50, 20};
    press.mouse_pressed   = true;
    press.mouse_down      = true;
    L::Sense before = {}, after = {};
    (void)L::layout(&early, press, zero, [&](L::Ui* u) {
        before = L::sense(u, "button");
        after  = button(u);
    });
    CHECK(before.clicked && before.hovered && before.held);
    CHECK(before.clicked == after.clicked && before.hovered == after.hovered && before.held == after.held);

    // Indexed ids are distinct from each other and from the bare name, and
    // stable across frames.
    CHECK(L::ElementId("row", 0) != L::ElementId("row", 1));
    CHECK(L::ElementId("row", 0) != L::ElementId("row"));
    L::Engine rows  = {};
    auto      build = [](L::Ui* u) {
        for (u32 index = 0; index < 3; ++index) {
            L::add(u, {.id = {"row", index}, .width = L::grow(), .height = L::px(20), .background = math::RED});
        }
    };
    L::Output* out = L::layout(&rows, ui_input(100, 100), zero, build);
    CHECK(out->duplicate_ids.len == 0);
    L::Input row_probe  = ui_input(100, 100);
    row_probe.mouse_pos = {50, 30}; // row 1 spans y = 20..40
    b32 hovered = false;
    (void)L::layout(&rows, row_probe, zero, [&](L::Ui* u) {
        hovered = L::sense(u, {"row", 1}).hovered;
        build(u);
    });
    CHECK(hovered);

    // Duplicate ids are reported once per extra occurrence.
    L::Engine dup = {};
    out = L::layout(&dup, ui_input(100, 100), zero, [&](L::Ui* u) {
        L::add(u, {.id = "dup", .width = L::px(10), .height = L::px(10)});
        L::add(u, {.id = "unique", .width = L::px(10), .height = L::px(10)});
        L::add(u, {.id = "dup", .width = L::px(10), .height = L::px(10)});
    });
    CHECK(out->duplicate_ids.len == 1 && out->duplicate_ids[0] == L::ElementId("dup"));
    out = L::layout(&dup, ui_input(100, 100), zero, [&](L::Ui* u) {
        L::add(u, {.id = "dup", .width = L::px(10), .height = L::px(10)});
        L::add(u, {.id = "unique", .width = L::px(10), .height = L::px(10)});
    });
    CHECK(out->duplicate_ids.len == 0);

    // Invisible layout containers don't capture the pointer; visible
    // primitives do.
    L::Engine pointer      = {};
    L::Input  outside      = ui_input(200, 100);
    outside.mouse_pos      = {150, 50};
    out = L::layout(&pointer, outside, zero, [&](L::Ui* u) {
        L::add_with(u, {.width = L::grow(), .height = L::grow()}, [&] {
            L::add(u, {.width = L::px(50), .height = L::px(50), .background = math::RED});
        });
    });
    CHECK(!out->is_pointer_over_ui);
    L::Input inside  = ui_input(200, 100);
    inside.mouse_pos = {25, 25};
    out = L::layout(&pointer, inside, zero, [&](L::Ui* u) {
        L::add(u, {.width = L::px(50), .height = L::px(50), .background = math::RED});
    });
    CHECK(out->is_pointer_over_ui);
    return true;
}

fn b32 test_ui_scroll() {
    namespace L = ui::layout;
    auto zero  = [](String, u16) { return L::TextMetrics{}; };
    auto build = [](L::Ui* u) {
        L::add_with(u,
                    {.id = "list", .width = L::px(100), .height = L::px(100),
                     .direction = L::Direction::TopToBottom, .scroll_y = true,
                     .border = {.width = 2, .color = math::BLUE}},
                    [&] {
                        const char* ids[] = {"row-a", "row-b", "row-c", "row-d"};
                        for (const char* id : ids) {
                            L::add(u, {.id = id, .width = L::grow(), .height = L::px(50), .background = math::RED});
                        }
                    });
    };

    L::Engine  engine = {};
    L::Output* out    = L::layout(&engine, ui_input(200, 200), zero, build);
    L::DrawKind expected[] = {L::DrawKind::ClipStart, L::DrawKind::Rectangle, L::DrawKind::Rectangle,
                              L::DrawKind::Rectangle, L::DrawKind::Rectangle, L::DrawKind::ClipEnd,
                              L::DrawKind::Border};
    CHECK(out->commands.len == 7);
    for (usize i = 0; i < 7; ++i) CHECK(out->commands[i].kind == expected[i]);
    // Fixed rows must not be compressed to fit the scrolling axis.
    CHECK(ui_find(out, "row-d")->bounds.y == 150);

    L::Input wheel  = ui_input(200, 200);
    wheel.mouse_pos = {50, 50};
    wheel.wheel     = {0, -30};
    out             = L::layout(&engine, wheel, zero, build);
    CHECK(ui_find(out, "row-a")->bounds.y == -30);

    // Content is 200 tall in a 100-tall viewport: the offset clamps at 100
    // no matter how far the wheel turns.
    L::Input flood  = ui_input(200, 200);
    flood.mouse_pos = {50, 50};
    flood.wheel     = {0, -1000};
    out             = L::layout(&engine, flood, zero, build);
    CHECK(ui_find(out, "row-a")->bounds.y == -100);

    // A row scrolled/clipped out of the viewport neither senses nor captures
    // the pointer: it sits where row-c lies before clipping (y = 100..150).
    L::Engine clipped = {};
    (void)L::layout(&clipped, ui_input(200, 200), zero, build);
    L::Input probe      = ui_input(200, 200);
    probe.mouse_pos     = {50, 125};
    probe.mouse_pressed = true;
    probe.mouse_down    = true;
    L::Sense sensed     = {.clicked = true, .hovered = true, .held = true};
    out                 = L::layout(&clipped, probe, zero, [&](L::Ui* u) {
        L::add_with(u,
                    {.id = "list", .width = L::px(100), .height = L::px(100),
                     .direction = L::Direction::TopToBottom, .scroll_y = true},
                    [&] {
                        const char* ids[] = {"row-a", "row-b", "row-c", "row-d"};
                        for (const char* id : ids) {
                            L::Sense sense =
                                L::add(u, {.id = id, .width = L::grow(), .height = L::px(50), .background = math::RED});
                            if (string::equals(id, "row-c")) sensed = sense;
                        }
                    });
    });
    CHECK(!sensed.clicked && !sensed.hovered && !sensed.held);
    CHECK(!out->is_pointer_over_ui);
    return true;
}

fn b32 test_ui_float() {
    namespace L = ui::layout;
    auto zero = [](String, u16) { return L::TextMetrics{}; };

    // Floats leave the flow and pin their anchor to the parent's, plus the
    // configured offset; they draw after the normal tree.
    L::Engine  engine = {};
    L::Output* out    = L::layout(&engine, ui_input(200, 100), zero, [&](L::Ui* u) {
        L::add_with(u, {.id = "panel", .width = L::px(100), .height = L::px(100)}, [&] {
            L::add(u, {.id = "flow", .width = L::grow(), .height = L::grow(), .background = math::RED});
            L::add(u, {.id = "float", .floating = true, .anchor_parent = {1, 1}, .anchor_self = {0, 0},
                       .float_offset = {5, 7}, .width = L::px(30), .height = L::px(20), .background = math::BLUE});
        });
    });
    CHECK((ui_find(out, "flow")->bounds == math::Rect{0, 0, 100, 100}));
    CHECK((ui_find(out, "float")->bounds == math::Rect{105, 107, 30, 20}));
    CHECK(out->commands[out->commands.len - 1].id == L::ElementId("float"));

    // Higher z draws later (on top), regardless of declaration order.
    L::Engine z = {};
    out = L::layout(&z, ui_input(100, 100), zero, [&](L::Ui* u) {
        L::add(u, {.id = "high", .floating = true, .z_index = 2, .width = L::px(10), .height = L::px(10),
                   .background = math::RED});
        L::add(u, {.id = "low", .floating = true, .z_index = 1, .width = L::px(10), .height = L::px(10),
                   .background = math::BLUE});
    });
    {
        usize high_at = 0, low_at = 0;
        for (usize i = 0; i < out->commands.len; ++i) {
            if (out->commands[i].id == L::ElementId("high")) high_at = i;
            if (out->commands[i].id == L::ElementId("low")) low_at = i;
        }
        CHECK(low_at < high_at);
    }

    // Floats escape ancestor clipping: one hanging below a scroll region
    // still senses and captures the pointer.
    L::Engine escape = {};
    auto      build  = [](L::Ui* u) {
        L::add_with(u,
                    {.id = "list", .width = L::px(100), .height = L::px(100),
                     .direction = L::Direction::TopToBottom, .scroll_y = true},
                    [&] {
                        const char* ids[] = {"row-a", "row-b", "row-c"};
                        for (const char* id : ids) {
                            L::add(u, {.id = id, .width = L::grow(), .height = L::px(50), .background = math::RED});
                        }
                        // Hangs below the container: y = 130..150.
                        L::add(u, {.id = "float", .floating = true, .anchor_parent = {0, 1}, .anchor_self = {0, 0},
                                   .float_offset = {10, 30}, .width = L::px(40), .height = L::px(20),
                                   .background = math::BLUE});
                    });
    };
    (void)L::layout(&escape, ui_input(200, 200), zero, build);
    L::Input probe  = ui_input(200, 200);
    probe.mouse_pos = {20, 140};
    L::Output* second = L::layout(&escape, probe, zero, build);
    CHECK(second->is_pointer_over_ui);

    // The tooltip shape: a floating bubble anchored to a text leaf. Text
    // sizing and children are orthogonal; a float touches neither flow nor
    // size.
    L::Engine tooltip = {};
    out = L::layout(&tooltip, ui_input(100, 100), [](String, u16) { return L::TextMetrics{{40, 10}, 10}; },
                    [&](L::Ui* u) {
                        L::add_with(u, {.id = "host", .text = {.text = "host", .size = 5}}, [&] {
                            L::add(u, {.id = "bubble", .floating = true, .anchor_parent = {0.5f, 0},
                                       .anchor_self = {0.5f, 1}, .width = L::px(20), .height = L::px(6),
                                       .background = math::BLUE});
                        });
                    });
    CHECK((ui_find(out, "host")->bounds == math::Rect{0, 0, 40, 10}));
    CHECK((ui_find(out, "bubble")->bounds == math::Rect{10, -6, 20, 6}));
    return true;
}

fn b32 test_ui_emit() {
    namespace L = ui::layout;

    // Emission order: background, text, children, then border on top.
    L::Engine  engine = {};
    L::Output* out    = L::layout(&engine, ui_input(100, 100),
                                  [](String, u16 size) { return L::TextMetrics{{20, (f32)size}, (f32)size}; },
                                  [&](L::Ui* u) {
                                      L::add_with(u,
                                                  {.id = "panel", .width = L::grow(), .height = L::grow(),
                                                   .background = math::RED,
                                                   .border = {.width = 2, .color = math::BLUE}},
                                                  [&] {
                                                      L::add(u, {.text = {.text = "child", .size = 10,
                                                                          .color = math::GREEN}});
                                                  });
                                  });
    CHECK(out->commands.len == 3);
    CHECK(out->commands[0].kind == L::DrawKind::Rectangle);
    CHECK(out->commands[1].kind == L::DrawKind::Text);
    CHECK(out->commands[2].kind == L::DrawKind::Border && out->commands[2].border_width == 2);

    // Corner radius rides on both the background and the border.
    auto      zero    = [](String, u16) { return L::TextMetrics{}; };
    L::Engine rounded = {};
    out = L::layout(&rounded, ui_input(100, 100), zero, [&](L::Ui* u) {
        L::add(u, {.id = "rounded", .width = L::px(50), .height = L::px(40), .background = math::RED,
                   .border = {.width = 2, .color = math::BLUE, .radius = 8}});
    });
    CHECK(ui_find_kind(out, "rounded", L::DrawKind::Rectangle)->corner_radius == 8);
    CHECK(ui_find_kind(out, "rounded", L::DrawKind::Border)->corner_radius == 8);

    // Commands own their text: the source string can die before rendering.
    L::Engine borrow = {};
    {
        char temporary[10];
        memcpy(temporary, "temporary", 9);
        (void)L::layout(&borrow, ui_input(100, 100),
                        [](String text, u16) { return L::TextMetrics{{(f32)text.len, 10}, 10}; },
                        [&](L::Ui* u) { L::add(u, {.text = {.text = {9, temporary}, .size = 10}}); });
        memset(temporary, 0, sizeof(temporary));
    }
    CHECK(string::equals(borrow.output.commands[0].text, "temporary"));

    // Images: fit takes the source size; explicit sizes stretch; untinted
    // images default to opaque white; the background draws beneath.
    L::Engine images = {};
    out = L::layout(&images, ui_input(200, 100), zero, [&](L::Ui* u) {
        L::add(u, {.id = "natural", .image = {7}, .image_source = {40, 30}, .background = math::RED});
        L::add(u, {.id = "stretched", .width = L::px(80), .height = L::px(20), .image = {7},
                   .image_source = {40, 30}, .image_tint = math::GREEN});
    });
    {
        const L::DrawCommand* natural = ui_find_kind(out, "natural", L::DrawKind::Image);
        CHECK(natural && natural->image == L::ImageId{7});
        CHECK(natural->bounds.w == 40 && natural->bounds.h == 30);
        CHECK((natural->color == math::Color{255, 255, 255, 255}));
        usize backdrop_at = (usize)-1, image_at = (usize)-1;
        for (usize i = 0; i < out->commands.len; ++i) {
            if (out->commands[i].kind == L::DrawKind::Rectangle && backdrop_at == (usize)-1) backdrop_at = i;
            if (out->commands[i].kind == L::DrawKind::Image && image_at == (usize)-1) image_at = i;
        }
        CHECK(backdrop_at < image_at);
        const L::DrawCommand* stretched = ui_find_kind(out, "stretched", L::DrawKind::Image);
        CHECK(stretched->bounds.w == 80 && stretched->bounds.h == 20 && stretched->color == math::GREEN);
    }

    // Fade scales the (tint's) alpha; a fully faded image emits nothing but
    // its background still draws.
    L::Engine fade = {};
    out = L::layout(&fade, ui_input(300, 100), zero, [&](L::Ui* u) {
        L::add(u, {.id = "half", .image = {7}, .image_source = {40, 30}, .image_fade = 0.5f});
        L::add(u, {.id = "tinted-half", .image = {7}, .image_source = {40, 30}, .image_tint = math::RED,
                   .image_fade = 0.5f});
        L::add(u, {.id = "gone", .image = {7}, .image_source = {40, 30}, .image_fade = 1.0f,
                   .background = math::BLUE});
    });
    CHECK((ui_find_kind(out, "half", L::DrawKind::Image)->color == math::Color{255, 255, 255, 127}));
    {
        math::Color tinted = ui_find_kind(out, "tinted-half", L::DrawKind::Image)->color;
        CHECK(tinted.r == math::RED.r && tinted.g == math::RED.g && tinted.b == math::RED.b && tinted.a == 127);
    }
    CHECK(ui_find_kind(out, "gone", L::DrawKind::Image) == 0);
    CHECK(ui_find_kind(out, "gone", L::DrawKind::Rectangle)->color == math::BLUE);
    return true;
}

// ------------------------------------------------------------ ui style/ir/run
// Ported from the Rust ui crate's style.rs / ir.rs / run.rs test suites.

fn b32 str_contains(String haystack, String needle) {
    if (needle.len > haystack.len) return false;
    if (needle.len == 0) return true;
    for (usize i = 0; i + needle.len <= haystack.len; ++i) {
        if (memcmp(haystack.data + i, needle.data, needle.len) == 0) return true;
    }
    return false;
}

fn b32 any_contains(Slice<String> messages, String needle) {
    for (String message : messages) {
        if (str_contains(message, needle)) return true;
    }
    return false;
}

// One seg of a node's text, checked against the expected (literal, var).
fn b32 seg_is(ui::ir::Text text, usize index, String literal, String var) {
    if (index >= text.segs.len) return false;
    return string::equals(text.segs[index].literal, literal) && string::equals(text.segs[index].var, var);
}

fn b32 test_ui_style() {
    namespace S = ui::style;
    arena::Arena a = {};
    arena::reserve(&a, 4 * 1024 * 1024);
    S::Style def = S::default_style();

    // Recognized keys override defaults; everything else keeps them.
    S::StyleModule parsed = S::parse(
        &a, "accent = { 1 0 0.5 }\ntooltip_background = { 0 0 0 0.5 }\ntext_size = 18\npadding = 6");
    CHECK(parsed.warnings.len == 0);
    CHECK(parsed.style.palette.accent == S::rgba(1, 0, 0.5f, 1));
    CHECK(parsed.style.tooltip_background == S::rgba(0, 0, 0, 0.5f));
    CHECK(parsed.style.text_size == 18 && parsed.style.padding == 6);
    CHECK(parsed.style.gap == def.gap && parsed.style.heading_size == def.heading_size);

    // Bad input warns per key and the default stands.
    S::StyleModule bad =
        S::parse(&a, "accent = 3\ngap = wide\nfrobnicate = 1\nink = { 0.1 0.2 }\nmuted = { 0.1 0.2 x }");
    CHECK(any_contains(bad.warnings, "'accent' is not a { r g b a } color"));
    CHECK(any_contains(bad.warnings, "'gap' is not a number"));
    CHECK(any_contains(bad.warnings, "unknown key 'frobnicate'"));
    CHECK(any_contains(bad.warnings, "'ink' is not a { r g b a } color"));
    CHECK(any_contains(bad.warnings, "'muted' is not a { r g b a } color"));
    CHECK(bad.style.palette.accent == def.palette.accent && bad.style.gap == def.gap);
    CHECK(bad.style.palette.ink == def.palette.ink && bad.style.palette.muted == def.palette.muted);

    // ZII: the empty source is the default style.
    S::StyleModule empty = S::parse(&a, "");
    CHECK(empty.warnings.len == 0 && empty.style.text_size == def.text_size);

    arena::release(&a);
    return true;
}

fn b32 test_ui_ir() {
    namespace I = ui::ir;
    namespace D = ui::data;
    arena::Arena a = {};
    arena::reserve(&a, 16 * 1024 * 1024);
    ui::style::Style style = ui::style::default_style();

    // Panels with properties; style values baked in at compile time.
    I::UiModule m = I::compile(
        &a, "panel = { x_pos = 0.1 y_pos = 0.5 border = yes direction = horizontal width = 120 label = \"a\" }",
        &style);
    CHECK(m.errors.len == 0 && m.warnings.len == 0);
    {
        I::UiNode panel = m.nodes[I::roots(&m)];
        CHECK(panel.floating && panel.border_width == 1 && panel.border_color == style.palette.outline);
        CHECK(approx(panel.x_pos, 0.1f) && approx(panel.y_pos, 0.5f));
        CHECK((panel.width == I::Size{120, false, 1}));
        CHECK(panel.direction == ui::layout::Direction::LeftToRight);
        CHECK(panel.padding.left == style.padding && panel.gap == style.gap);
        CHECK(panel.background.color == style.palette.panel && panel.background.name.segs.len == 0);
        I::UiNode label = m.nodes[panel.first_child];
        CHECK(seg_is(label.text, 0, "a", ""));
        CHECK(label.text_size == style.text_size && label.color.color == style.palette.ink);
        CHECK(label.next_sibling == 0);
    }

    // Logical sizes and layout properties; rows are transparent flush cells.
    m = I::compile(&a,
                   "panel = { width = 92% max_width = 760 align = center padding = 22 gap = 12 "
                   "row = { height = 54 "
                   "panel = { width = grow min_width = 70 tooltip = \"tip\" } "
                   "panel = { width = grow:2 background = accent } } }",
                   &style);
    CHECK(m.errors.len == 0 && m.warnings.len == 0);
    {
        I::UiNode panel = m.nodes[I::roots(&m)];
        CHECK(panel.width.fraction && approx(panel.width.cap, 0.92f) && panel.width.weight == 1);
        CHECK(panel.max_width == 760);
        CHECK(panel.align_x == ui::layout::Align::Center && panel.align_y == ui::layout::Align::Center);
        CHECK(panel.padding.left == 22 && panel.padding.bottom == 22 && panel.gap == 12);
        I::UiNode row = m.nodes[panel.first_child];
        CHECK(row.direction == ui::layout::Direction::LeftToRight);
        CHECK(row.width == I::GROW);
        CHECK((row.height == I::Size{54, false, 1}));
        CHECK(row.background.color.a == 0 && row.padding.left == 0);
        I::UiNode one = m.nodes[row.first_child];
        CHECK(one.width == I::GROW && one.min_width == 70 && seg_is(one.tooltip, 0, "tip", ""));
        I::UiNode two = m.nodes[one.next_sibling];
        CHECK(two.width.cap == 0 && two.width.weight == 2);
        CHECK(two.background.color == style.palette.accent);
    }

    // The cap[:weight] grammar covers every form; grow:0 is fit.
    m = I::compile(&a,
                   "panel = { panel = { width = \"180:2\" } panel = { width = \"50%:3\" } "
                   "panel = { width = \"grow:0\" } panel = { width = fit } }",
                   &style);
    CHECK(m.errors.len == 0 && m.warnings.len == 0);
    {
        I::UiNode root           = m.nodes[I::roots(&m)];
        I::UiNode capped_weighted = m.nodes[root.first_child];
        CHECK(capped_weighted.width.cap == 180 && capped_weighted.width.weight == 2);
        I::UiNode fraction = m.nodes[capped_weighted.next_sibling];
        CHECK(fraction.width.fraction && fraction.width.cap == 0.5f && fraction.width.weight == 3);
        I::UiNode fit_spelled = m.nodes[fraction.next_sibling];
        CHECK(fit_spelled.width == I::FIT);
        CHECK(m.nodes[fit_spelled.next_sibling].width == I::FIT);
    }

    // Bad sizes warn and fall back to the widget's default.
    m = I::compile(&a, "panel = { width = \"180:x\" height = \"fit:2\" min_width = 10 panel = { width = wide } }",
                   &style);
    CHECK(m.errors.len == 0);
    CHECK(any_contains(vec::slice(&m.warnings), "bad weight"));
    CHECK(any_contains(vec::slice(&m.warnings), "fit means weight 0"));
    CHECK(any_contains(vec::slice(&m.warnings), "cap[:weight]"));
    {
        I::UiNode panel = m.nodes[I::roots(&m)];
        CHECK(panel.width == I::FIT && panel.height == I::FIT); // floating top-level fits
        CHECK(m.nodes[panel.first_child].width == I::GROW);     // nested panels grow by default
    }

    // Boxes and text roles carry their style defaults.
    m = I::compile(&a, "panel = { heading = \"Big\" section = \"SMALL\" box = { min_width = 70 label = \"1x\" } }",
                   &style);
    CHECK(m.errors.len == 0 && m.warnings.len == 0);
    {
        I::UiNode panel   = m.nodes[I::roots(&m)];
        I::UiNode heading = m.nodes[panel.first_child];
        CHECK(heading.text_size == style.heading_size && heading.color.color == style.palette.ink);
        I::UiNode section = m.nodes[heading.next_sibling];
        CHECK(section.text_size == style.section_size && section.color.color == style.palette.muted);
        I::UiNode cell = m.nodes[section.next_sibling];
        CHECK(cell.width == I::GROW && cell.height == I::GROW);
        CHECK(cell.align_x == ui::layout::Align::Center && cell.align_y == ui::layout::Align::Center);
        CHECK(cell.background.color == style.palette.accent && cell.min_width == 70);
        CHECK(m.nodes[cell.first_child].text_size == style.text_size);
    }

    // $VAR tokenization: literals keep the source spelling as fallback.
    m = I::compile(&a,
                   "panel = { list = { id = l template = { "
                   "button = { id = \"element_$ID\" action = \"hire $ID\" text = \"$NAME!\" } } } }",
                   &style);
    {
        I::UiNode panel    = m.nodes[I::roots(&m)];
        I::UiNode list     = m.nodes[panel.first_child];
        I::UiNode template_node = m.nodes[list.template_node];
        I::UiNode button   = m.nodes[template_node.first_child];
        CHECK(seg_is(button.id, 0, "element_", ""));
        CHECK(seg_is(button.id, 1, "$ID", "ID"));
        CHECK(seg_is(button.action, 0, "hire ", ""));
        CHECK(seg_is(button.action, 1, "$ID", "ID"));
        I::UiNode caption = m.nodes[button.first_child];
        CHECK(seg_is(caption.text, 0, "$NAME", "NAME"));
        CHECK(seg_is(caption.text, 1, "!", ""));
    }

    // Conditions compile as tokenized text; bare names warn.
    m = I::compile(&a, "panel = { visible = \"$CHARACTER_OPEN\" label = { text = a visible = no } }", &style);
    CHECK(m.errors.len == 0 && m.warnings.len == 0);
    {
        I::UiNode panel = m.nodes[I::roots(&m)];
        CHECK(seg_is(panel.visible, 0, "$CHARACTER_OPEN", "CHARACTER_OPEN"));
        CHECK(seg_is(m.nodes[panel.first_child].visible, 0, "no", ""));
    }
    m = I::compile(&a, "panel = { visible = character_open }", &style);
    CHECK(any_contains(vec::slice(&m.warnings), "'visible = character_open' is not yes/no or a $VAR binding"));

    // Unknown keys and missing '=' warn without failing.
    m = I::compile(&a, "panel = { frobnicate = 3 panel { } }\nlabel = \"top level\"", &style);
    CHECK(any_contains(vec::slice(&m.warnings), "unknown key 'frobnicate'"));
    CHECK(any_contains(vec::slice(&m.warnings), "missing '='"));
    CHECK(any_contains(vec::slice(&m.warnings), "unknown key 'label'"));

    // The style bakes in; $VAR color names survive for per-frame resolution.
    ui::style::Style custom  = ui::style::default_style();
    custom.padding           = 5;
    custom.heading_size      = 40;
    custom.button_background = ui::style::rgba(1, 0, 0, 1);
    custom.button_hover      = ui::style::rgba(0, 1, 0, 1);
    m = I::compile(&a, "panel = { heading = \"H\" button = { action = a text = b } "
                       "panel = { background = \"$ROW_COLOR\" } }",
                   &custom);
    CHECK(m.warnings.len == 0);
    {
        I::UiNode panel = m.nodes[I::roots(&m)];
        CHECK(panel.padding.left == 5);
        I::UiNode heading = m.nodes[panel.first_child];
        CHECK(heading.text_size == 40);
        I::UiNode button = m.nodes[heading.next_sibling];
        CHECK(button.background.color == custom.button_background);
        CHECK(button.hover_background == custom.button_hover);
        I::UiNode dynamic = m.nodes[button.next_sibling];
        CHECK(seg_is(dynamic.background.name, 0, "$ROW_COLOR", "ROW_COLOR"));
        CHECK(dynamic.background.color == custom.palette.panel);
        CHECK(m.palette.accent == custom.palette.accent);
    }

    // Unknown palette names warn; the default stands.
    m = I::compile(&a, "panel = { background = acent }", &style);
    CHECK(any_contains(vec::slice(&m.warnings), "'background = acent' is not a palette color"));
    CHECK(m.nodes[I::roots(&m)].background.color == style.palette.panel);

    // Recovers around parse errors; ZII empty module.
    m = I::compile(&a, "panel = { label = \"ok\" ", &style);
    CHECK(m.errors.len > 0);
    {
        I::UiNode panel = m.nodes[I::roots(&m)];
        CHECK(panel.floating);
        CHECK(seg_is(m.nodes[panel.first_child].text, 0, "ok", ""));
    }
    m = I::compile(&a, "", &style);
    CHECK(I::roots(&m) == 0 && m.errors.len == 0 && m.warnings.len == 0);

    // UiData: globals bind outside the row machinery.
    D::Data data = D::make(&a);
    D::begin_list(&data, "l");
    D::begin_row(&data);
    D::bind(&data, "A", "row");
    D::bind_global(&data, "STATUS", "Year 700"); // legal mid-fill
    D::bind(&data, "B", "row");
    D::Row row = D::rows(&data, data.lists[0])[0];
    CHECK(D::bindings(&data, row).len == 2 && data.globals.len == 1);
    CHECK(string::equals(data.globals[0].key, "STATUS"));
    CHECK(string::equals(data.globals[0].value, "Year 700"));

    arena::release(&a);
    return true;
}

fn b32 test_ui_run() {
    namespace I = ui::ir;
    namespace D = ui::data;
    namespace L = ui::layout;
    arena::Arena a = {};
    arena::reserve(&a, 16 * 1024 * 1024);
    arena::Arena frame = {};
    arena::reserve(&frame, 4 * 1024 * 1024);
    ui::style::Style style = ui::style::default_style();
    auto             zero  = [](String, u16) { return L::TextMetrics{}; };

    // Clicks produce interpolated events, in click order; action-less
    // buttons swallow the click.
    I::UiModule module = I::compile(&a,
                                    "panel = {\n"
                                    "    button = { action = \"print OK\" text = go width = 100 height = 30 }\n"
                                    "    button = { text = mute width = 100 height = 30 }\n"
                                    "    list = {\n"
                                    "        id = \"people_list\"\n"
                                    "        template = {\n"
                                    "            button = { id = \"element_$ID\" action = \"hire $ID\" "
                                    "text = \"$NAME\" width = 100 height = 30 }\n"
                                    "        }\n"
                                    "    }\n"
                                    "}",
                                    &style);
    CHECK(module.errors.len == 0 && module.warnings.len == 0);

    D::Data people = D::make(&a);
    D::begin_list(&people, "people_list");
    D::begin_row(&people);
    D::bind(&people, "ID", "7");
    D::bind(&people, "NAME", "Livia");

    L::Engine engine   = {};
    auto      click_at = [&](const I::UiModule* m, const D::Data* data, f32 x, f32 y, b32 pressed) {
        arena::reset(&frame);
        L::Input probe      = ui_input(400, 400);
        probe.mouse_pos     = {x, y};
        probe.mouse_pressed = pressed;
        Slice<String> events = {};
        (void)L::layout(&engine, probe, zero,
                        [&](L::Ui* u) { events = ui::walk::run(m, data, &frame, u); });
        return events;
    };

    // Panel padding 12, gap 8: buttons at y = 12 and 50, the list's row at
    // y = 88. A frame with no input establishes the bounds sense reads on
    // the next one.
    CHECK(click_at(&module, &people, 0, 0, false).len == 0);
    {
        Slice<String> events = click_at(&module, &people, 50, 25, true);
        CHECK(events.len == 1 && string::equals(events[0], "print OK"));
    }
    CHECK(click_at(&module, &people, 50, 60, true).len == 0);
    {
        Slice<String> events = click_at(&module, &people, 50, 95, true);
        CHECK(events.len == 1 && string::equals(events[0], "hire 7"));
    }

    // Hidden nodes are not declared: the conditional button only exists
    // once its binding says yes — and then it is the panel's first child.
    I::UiModule conditional = I::compile(
        &a,
        "panel = {\n"
        "    button = { action = never text = a visible = no width = 100 height = 30 }\n"
        "    button = { action = maybe text = b visible = \"$WINDOW_OPEN\" width = 100 height = 30 }\n"
        "}",
        &style);
    CHECK(conditional.errors.len == 0 && conditional.warnings.len == 0);
    L::Engine hidden_engine = {};
    engine                  = hidden_engine; // fresh caches for the new module
    D::Data closed        = D::make(&a);
    CHECK(click_at(&conditional, &closed, 0, 0, false).len == 0);
    CHECK(click_at(&conditional, &closed, 50, 25, true).len == 0);
    D::Data open = D::make(&a);
    D::bind_global(&open, "WINDOW_OPEN", "yes");
    CHECK(click_at(&conditional, &open, 0, 0, false).len == 0);
    {
        Slice<String> events = click_at(&conditional, &open, 50, 25, true);
        CHECK(events.len == 1 && string::equals(events[0], "maybe"));
    }

    // Disabled buttons occupy their slot but sense nothing; unbound
    // conditions ($CAN_PRESS stays literal) disable too.
    I::UiModule gated = I::compile(
        &a,
        "panel = { button = { action = press text = a enabled = \"$CAN_PRESS\" width = 100 height = 30 } }",
        &style);
    CHECK(gated.errors.len == 0 && gated.warnings.len == 0);
    L::Engine gated_engine = {};
    engine                 = gated_engine;
    D::Data unbound      = D::make(&a);
    CHECK(click_at(&gated, &unbound, 50, 25, false).len == 0);
    CHECK(click_at(&gated, &unbound, 50, 25, true).len == 0);
    D::Data off = D::make(&a);
    D::bind_global(&off, "CAN_PRESS", "no");
    CHECK(click_at(&gated, &off, 50, 25, false).len == 0);
    CHECK(click_at(&gated, &off, 50, 25, true).len == 0);
    D::Data on = D::make(&a);
    D::bind_global(&on, "CAN_PRESS", "yes");
    CHECK(click_at(&gated, &on, 50, 25, false).len == 0);
    {
        Slice<String> events = click_at(&gated, &on, 50, 25, true);
        CHECK(events.len == 1 && string::equals(events[0], "press"));
    }
    return true;
}

// Corpus: the repo's real data files must load clean through the facade —
// the equivalent of the Rust crate's integration test, and the first of the
// corpus tests CLAUDE.md calls for. Also runs one headless frame end to
// end: load -> bind -> run -> self-contained Result.
fn b32 test_ui_corpus() {
    arena::Arena frame = {};
    arena::reserve(&frame, 16 * 1024 * 1024);

    ui::Ui        hud      = {};
    Slice<String> problems = ui::load(&hud, "data/ui.txt", "data/style.txt");
    for (String problem : problems) printf("\n  %.*s", (int)problem.len, problem.data);
    CHECK(problems.len == 0);
    CHECK(ui::ir::roots(&hud.module) != 0);

    ui::data::Data data = ui::data::make(&frame);
    ui::data::bind_global(&data, "STATUS", "corpus");
    ui::data::bind_global(&data, "DATE", "Year 700");
    ui::data::begin_list(&data, "speeds");
    ui::data::begin_row(&data);
    ui::data::bind(&data, "LEVEL", "1");
    ui::data::bind(&data, "ENABLED", "yes");

    ui::Result result = ui::run(&hud, &frame, ui_input(1280, 720), &data,
                                [](String text, u16) { return ui::TextMetrics{{(f32)text.len * 8, 16}, 16}; });
    CHECK(result.commands.len > 0); // the visible panels drew something
    CHECK(result.actions.len == 0); // nothing was clicked

    // The Result is self-contained: reloading the module must not disturb it.
    String first_text = {};
    for (const ui::DrawCommand& command : result.commands) {
        if (command.kind == ui::DrawKind::Text) {
            first_text = command.text;
            break;
        }
    }
    CHECK(first_text.len > 0);
    (void)ui::load(&hud, "data/ui.txt", "data/style.txt");
    CHECK(first_text.len > 0 && first_text.data[0] != 0); // still readable after reload

    arena::release(&frame);
    return true;
}

struct Test {
    const char* name;
    b32 (*func)();
};

const Test TESTS[] = {
    {"arena", test_arena},
    {"vec", test_vec},
    {"list", test_list},
    {"string", test_string},
    {"file_io", test_file_io},
    {"tabula", test_tabula},
    {"tabula_errors", test_tabula_errors},
    {"pool", test_pool},
    {"math", test_math},
    {"hashtable", test_hashtable},
    {"ui_grow", test_ui_grow},
    {"ui_compress", test_ui_compress},
    {"ui_fit", test_ui_fit},
    {"ui_text", test_ui_text},
    {"ui_sense", test_ui_sense},
    {"ui_scroll", test_ui_scroll},
    {"ui_float", test_ui_float},
    {"ui_emit", test_ui_emit},
    {"ui_style", test_ui_style},
    {"ui_ir", test_ui_ir},
    {"ui_run", test_ui_run},
    {"ui_corpus", test_ui_corpus},
};

} // namespace

int main() {
    int total  = (int)(sizeof(TESTS) / sizeof(TESTS[0]));
    int failed = 0;
    for (int i = 0; i < total; ++i) {
        // Name goes out before the test runs, so a crash names its culprit.
        printf("%s ...", TESTS[i].name);
        fflush(stdout);
        if (TESTS[i].func()) {
            printf(" ok\n");
        } else {
            printf("\n");
            failed += 1;
        }
    }
    printf("%d/%d tests passed\n", total - failed, total);
    return failed;
}
