// ROOT: the test suite — dense tripwire tests for core and data modules.
// Tests exist to trip, not to specify: each function exercises many behaviors
// at once, and CHECK's file:line output is what makes a trip diagnosable.
#include "core.hpp"
#include "arena.hpp"
#include "file_io.hpp"
#include "list.hpp"
#include "math.hpp"
#include "pool.hpp"
#include "string.hpp"
#include "vec.hpp"
#include "tabula.hpp"

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
    // the repo root — run via ./build.sh test.
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
    u32 slot;
    u32 generation;
};

fn b32 test_pool() {
    pool::Pool<TestKey, u64, 4> p = {}; // slot 0 is the dummy; 3 usable
    TestKey null_key = {};
    CHECK(p.live_count == 0 && &p[null_key] == &p.entries[0].value); // ZII: null key -> dummy
    for (auto& entry : p) {
        (void)entry;
        CHECK(false); // zero pool iterates nothing
    }

    TestKey a = pool::alloc(&p);
    TestKey b = pool::alloc(&p);
    TestKey c = pool::alloc(&p);
    CHECK(a.slot == 1 && b.slot == 2 && c.slot == 3 && p.live_count == 3);
    CHECK(pool::is_nil_key(null_key) && !pool::is_nil_key(a));
    CHECK((a.generation & 1) == 1 && p[a] == 0); // live keys are odd; fresh slots zeroed
    CHECK(pool::alloc(&p).slot == 0);            // full -> zero key, no trap

    p[b] = 42;
    p[c] = 5;
    CHECK(p.entries[2].value == 42 && p[b] == 42);

    CHECK(pool::free(&p, b) && !pool::free(&p, b)); // b32 result: freed once, stale after
    CHECK(!pool::free(&p, null_key) && !pool::free(&p, TestKey{99, 1}));
    CHECK(&p[b] == &p.entries[0].value && p.live_count == 2); // stale -> dummy

    TestKey b2 = pool::alloc(&p); // slot reused under a fresh generation
    CHECK(b2.slot == 2 && b2.generation != b.generation && p[b2] == 0); // zeroed on reuse
    CHECK(&p[b] == &p.entries[0].value); // old key still stale
    p[b2] = 7;

    pool::free(&p, a);
    u64 sum  = 0;
    u32 seen = 0;
    for (auto& entry : p) { // live slots only: dummy and freed slot skipped
        CHECK(entry.key.slot != 0 && (entry.key.generation & 1) == 1);
        sum += entry.value;
        seen += 1;
    }
    CHECK(seen == 2 && sum == 12); // b2 (7) + c (5)

    TestKey d = pool::insert(&p, (u64)30); // alloc + assign in one step, reusing freed slot 1
    CHECK(!pool::is_nil_key(d) && d.slot == 1 && p[d] == 30 && p.live_count == 3);
    CHECK(pool::is_nil_key(pool::insert(&p, (u64)31)));    // full again -> nil key
    CHECK(p.entries[0].value == 0 && p.entries[0].key.generation == 0); // dummy untouched

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
