# imperium-cpp — Style Guide

This document defines how all code in this project is written. Follow it closely.
When in doubt, prefer the simplest thing that works: plain data, plain functions,
memory managed in bulk. If a rule here conflicts with a habit from "modern C++",
this document wins.

## Philosophy

- Write C-style C++: structs of plain data, and free functions — grouped in flat
  per-module namespaces — that operate on them.
- **ZII — Zero Is Initialization.** The all-zero value of every type is a valid,
  meaningful default. Code never needs a constructor to be in a usable state.
- **Allocate in bulk.** Memory comes from arenas or static buffers, sized per
  system or per lifetime — never from fine-grained per-object allocation.
- **Fat structs, flat data.** Prefer one large struct with embedded fixed-size
  arrays over graphs of small heap objects linked by pointers.
- No hidden control flow, no hidden allocation. What happens at a call site is
  what you can read at the call site.

## Language subset

Compile as C++20 with `-fno-exceptions -fno-rtti`. Treat warnings as errors.

**Use:**
- `fn` — the project function keyword (`#define fn inline` in `core.hpp`):
  every free-function definition is written `fn <return> name(...)`, template
  functions included (`template <typename T> fn T* allocate(...)`). It makes
  every module safe to include from more than one TU of a binary (the
  boundary TU). Not annotated: `main` (may not be inline), declarations
  (module contracts like `ray.hpp` stay plain), in-class member bodies
  (implicitly inline already), and boundary-TU implementations of a module's
  API — those must keep a single strong definition other TUs link against.
- `struct` with all members public; `enum class`; `union`; free functions
- namespaces — one namespace per module; a directory of modules is one
  module with nested namespaces (`ui::layout`, `ui::ir`); anonymous
  namespaces for module-internal helpers (see *Namespaces*)
- templates, in a restrained way (containers, math, small generic utilities — see below)
- lambdas passed as template callable parameters and invoked during the
  call — the monomorphized equivalent of a function pointer + context, used
  for injected callbacks (`measure_text`) and scoped bodies
  (`ui::layout::add_with`). Never stored in a struct, never type-erased,
  never escaping the callee. Simple concepts / `requires` clauses may spell
  the callable's contract (`MeasureText`, `ElementBody`) or a template
  parameter's data shape (`pool::Key` — a POD wrapping one u64 `value`);
  nothing beyond that.
- operator overloading for arithmetic value types only (vectors, matrices)
- overload sets — the same function name across related types is fine
  (`length(V2)`, `length(V3)`)
- `auto` where it keeps code readable; spell the type out when it isn't obvious
  from the right-hand side
- `constexpr` for constants and simple compile-time values
- function pointers where runtime polymorphism is genuinely needed

**Never use:**
- exceptions, RTTI (`dynamic_cast`, `typeid`), `throw`/`try`/`catch`
- inheritance, virtual functions, abstract classes
- destructors, RAII, copy/move special members — every type stays trivially
  copyable and trivially destructible. Small convenience constructors are fine
  (`String(const char*)`, `V2(f32, f32)`) as long as the type keeps a
  `= default` default constructor and its all-zero state stays valid; no
  constructor may acquire resources or be required for correctness. The one
  sanctioned RAII exception is `arena::ScratchArena` (see *Memory*); nothing
  else earns a destructor without a conversation.
- member functions — write `world::entity_update(Entity* entity)` instead of
  `entity.update()`. Exceptions: operator overloads on math types, the small
  convenience constructors described above, and `begin`/`end` on container
  types (`Array`, `Slice`) so range-for works.
- `using namespace`, at any scope — see *Namespaces*
- `new`/`delete`, `malloc`/`free` outside the memory layer, smart pointers
- `private`/`protected`, `friend`, `class` keyword (use `struct`)
- `std::function`, functors, and lambdas outside the sanctioned callable-
  parameter shape above (no storing closures, no type erasure)
- references as everyday parameters, returns, or fields — pass pointers, so
  mutation is visible at the call site. References are allowed where they are
  genuinely necessary (`operator[]` returning `T&` in a container, operator
  overload parameters).
- multiple return via `std::tuple`/structured bindings — return a named result struct

**Standard library:** avoided by default. Permitted includes:
- `<stdint.h>`, `<stddef.h>` — behind `core.hpp` only, for the primitive aliases
- `<string.h>` — `memcpy`, `memset`, `memcmp`, `memmove`, `strlen`
- `<math.h>` — scalar math
- `<stdarg.h>` — varargs for formatting functions
- `<compare>` — only for defaulted comparison operators (`operator<=>`) on
  value types
- `<stdio.h>`, `<stdlib.h>` — platform/debug layer only (startup allocation,
  logging), plus one sanctioned exception: `vsnprintf` as the backend of
  `string::format`
- OS headers (`sys/mman.h`, `windows.h`, ...) — inside the platform-guarded
  implementation blocks of platform modules only, never at module top level
- `<atomic>` — only if/when threading requires it
- Anything else (containers, `<string>`, `<memory>`, `<algorithm>`, `<vector>`,
  iostreams, `<chrono>`, ranges, ...) is off-limits. If a need comes up, raise it
  in conversation instead of including it.

## Namespaces

Namespaces are the organization and collision-avoidance tool — there are no
name prefixes.

- One namespace per module, named after the module, `snake_case`. A module
  is usually one file and one flat namespace; a directory under `src/` is
  one module split across files, and its files nest inside the directory's
  namespace — `src/ui/layout.hpp` is `ui::layout`, `src/ui/ir.hpp` is
  `ui::ir`. Nesting mirrors the file tree, never invents extra levels.
  A type shared by the whole directory may sit at the directory namespace
  itself.
- A module's functions **and** its types live in its namespace: `arena::Arena`,
  `arena::push`, `world::Entity`, `world::spawn`, `renderer::flush`.
- Exception: everything defined in `core.hpp` is global — the primitive
  aliases, `ASSERT`, `min`/`max`, `Array`, `Slice`, `String`. Nothing else is.
- Module-internal helpers and globals go in an anonymous namespace inside the
  module. Everything named in the module's namespace is that module's public
  API — the split is visible in the file.
- `using namespace` is banned at every scope. Write the qualifier at every
  cross-module call site. If an unqualified cross-module call compiles anyway,
  that's ADL — treat it as a bug, not a convenience. Operator overloads are the
  one sanctioned ADL lookup. One exception: a module may write
  `using namespace math;` inside its own namespace block — math value types
  are pervasive enough to earn it (decided for `ray.hpp`, extended to the
  `ui` modules). Note the leak: the imported names become reachable through
  the module's namespace (`ray::V2` spells). No other namespace gets this.
- Decided: math value types live in `math.hpp` as `math::V2`, `math::Rect`,
  `math::Color` (plus the color constants), ... — not in `core.hpp`.

## Primitive types

All code uses these aliases, defined once in `src/core.hpp`. Never spell out
`unsigned int`, `int64_t`, `size_t`, etc. in project code.

```cpp
using u8    = uint8_t;
using u16   = uint16_t;
using u32   = uint32_t;
using u64   = uint64_t;
using i8    = int8_t;
using i16   = int16_t;
using i32   = int32_t;
using i64   = int64_t;
using f32   = float;
using f64   = double;
using usize = size_t;
using isize = ptrdiff_t;
using b32   = i32;      // "boolean" in structs — fixed size, ZII-friendly
```

`bool` is fine for locals and return values; use `b32` inside serialized or
memory-mapped structs where size must be explicit.

## PODs only

Every type in this codebase is a POD — trivially copyable, trivially
destructible, standard layout. This is load-bearing: it is what makes ZII,
`memcpy`, arena clearing, and memory-mapped/serialized I/O all safe.

- The default: any struct you write is a POD. No reference fields, no virtual
  anything, no members that are themselves non-POD. Small convenience
  constructors don't break this — the invariants that matter are trivially
  copyable, trivially destructible, and a valid all-zero state.
- If a type genuinely cannot be POD (rare — typically something in the platform
  layer wrapping an OS resource), mark it loudly at the definition site and say
  what is unsafe:

```cpp
// NOT POD — do not memcpy or zero-reset; lifecycle owned by window::open/close.
struct Window { ... };
```

- For types where it matters (anything serialized, pooled, or bulk-copied),
  enforce it: `STATIC_ASSERT_POD(T)` in `core.hpp`, wrapping
  `static_assert(std::is_trivially_copyable_v<T>)`.

## ZII — Zero Is Initialization

Every type must be designed so that all-zero means "empty / none / default /
identity" — whichever is most natural for that type.

- Initialize with `= {};`, reset with `memset` or by assigning `{}`. Arena
  allocations come back zeroed, so a pushed struct is already valid.
- The first value of every enum is zero, named `Nil`, and means "none/invalid":

```cpp
enum class UnitKind : u32 {
    Nil,        // zero — ZII default
    Legion,
    Fleet,
    Count,
};
```

- Reserve index/handle 0 as the null handle. Slot 0 of pooled arrays is a
  permanently-zeroed dummy so lookups of the null handle return valid empty data.
- Functions signal failure by returning the zero value: an empty `String`, a
  zero handle, a zeroed result struct. Callers test against zero/empty:

```cpp
world::Entity* entity = world::entity_from_handle(w, handle);
if (entity->kind == world::UnitKind::Nil) { /* not found — safe to read, all zero */ }
```

- Never invent a second sentinel (`-1`, `0xFFFFFFFF`, magic values). Zero is the
  one and only "nothing".
- If a field's natural default isn't zero, encode it so zero maps to that
  default (e.g. store `scale_minus_one` — better: pick representations where
  zero *is* the default).

## Fat structs

- Each system's entire state lives in one struct: `renderer::Renderer`,
  `world::World`, `assets::Store`, `input::Input`. Systems are explicit —
  functions take `System* system` as their first parameter. No hidden
  singletons; the app root struct owns every system and wires them together in
  the root file.
- Prefer embedded fixed-size arrays with a count over dynamic structures:

```cpp
namespace world {

struct World {
    Entity entities[MAX_ENTITIES];
    u32    entity_count;

    Province provinces[MAX_PROVINCES];
    u32      province_count;
};

}
```

- Cross-references between long-lived objects are indices or generation handles
  (`u32` / `{u32 index; u32 generation;}`), not pointers. Pointers are fine as
  short-lived locals within a function or frame.
- Don't prematurely shrink or split structs to save memory. Memory is budgeted
  per system up front; a fat struct with headroom beats a web of allocations.

## Memory

The arena is the allocator. There is no general-purpose heap use in gameplay or
systems code.

```cpp
namespace arena {

struct Arena {
    u8*   base;      // null = empty/failed arena (ZII)
    usize size;      // reserved capacity
    usize used;
    usize committed; // bytes backed by committed pages
};

void reserve(Arena* arena, usize capacity);                 // ZII: stays zero on failure
u8*  allocate_raw(Arena* arena, usize length, usize align); // zeroed; null when it doesn't fit
void reset(Arena* arena);
void release(Arena* arena);

template <typename T> T* allocate(Arena* arena, usize count = 1);

}
```

- **All pushed memory is zeroed** — this is what makes ZII free at the use site.
- Arenas are carved out of one (or few) large OS allocations at startup, backed
  by `vmem::` (reserve/commit). The platform modules are the only place
  `malloc`/`mmap`/`VirtualAlloc` appears.
- Lifetimes are explicit and bulk:
  - `permanent_arena` — lives for the whole run (assets, world state backing).
  - `frame_arena` — cleared at the top of every frame.
  - per-system arenas — owned by the system struct, cleared on system reset.
  - scratch arenas — `arena::ScratchArena scratch(&frame_arena);` restores
    `used` at scope exit, reclaiming everything allocated inside the scope
    even on early return. The codebase's one RAII type: stack-local only,
    never stored, copied, or serialized.
- **Nothing is freed individually.** Cleanup means clearing or resetting an
  arena. If you're writing a `foo_destroy` that walks objects to release them
  one by one, the design is wrong.
- Truly fixed-size, known-at-compile-time data may simply live as globals in
  the module's anonymous namespace or be embedded in the owning system struct
  instead of arena-pushed.

## Strings

- The string type is a view — pointer + length, not null-terminated. It is
  its own struct, distinct from `Slice<char>`: its bytes are `const` (never
  written through a String), and it carries the three sanctioned implicit
  conversions — from a null-terminated literal, from `Slice<char>`, and from
  `DynString<N>` (the fixed-capacity inline string in core), so builders
  assemble bytes in a mutable slice or inline buffer and the result travels
  as String:

```cpp
struct String {
    usize       len;
    const char* data;

    String() = default;
    String(usize len, const char* data) : len{len}, data{data} {}
    String(const char* cstr) : len{cstr ? strlen(cstr) : 0}, data{cstr} {}
    String(Slice<char> s) : len{s.len}, data{s.data} {}
    template <const usize N> String(const DynString<N>& s);
};
// usage: String name = "Roma";
```

- `DynString<N>` is the string counterpart of `DynArray<T, N>`: embedded
  fixed-capacity bytes + length, ZII (all-zero = empty string), no growth —
  `push`/`append` return b32 and leave the contents untouched on overflow.
  Assignment from String (`title = "Roma";`) copies bytes in and truncates
  to capacity silently — decided; use `append` where refusal must be
  visible. The operator is not a copy/move special member, so the type
  stays trivially copyable.
  It is what lets string data live inline in POD state (World, serialized
  structs). The String conversion views the buffer it converted from: don't
  let such a String outlive its DynString — in particular, never convert
  from a function's temporary return value into a String that gets stored.

- ZII: the empty string is `{}` — null data, zero count. All string functions
  must accept it.
- Strings are immutable views or arena-allocated copies. Building strings means
  pushing into an arena, not mutating in place.
- Null-terminated C strings exist only at OS/library boundaries; convert at the
  boundary (`str::to_cstr(arena, s)`), never let them leak inward.

## Containers

In order of preference:
1. Fixed-size embedded array + count (see fat structs) — raw `T arr[N]`, the
   bounds-asserted `Array<T, N>`, or the fixed-capacity `DynArray<T, N>` /
   `DynString<N>` from core when the count varies at runtime.
2. Arena-allocated array when the count is only known at runtime but fixed
   after creation: `arena::allocate<Entity>(a, count)`.
3. `Slice<T>` — the universal view type for passing ranges around.
4. Hand-rolled, arena-backed structures written once in a core-adjacent
   module and reused — only when 1–3 truly don't fit. Existing: `vec::Vec`
   (growable array), `list::List` (chunked, address-stable), `pool::Pool`
   (generational handles behind opaque u64 keys — the slot/generation
   packing is private to the module), `hashtable::Table` (open-addressing hash map,
   u64 keys, key 0 reserved; value pointers are invalidated by growth — an
   address-stable binned variant would be a separate module beside it).

No `std::vector`, `std::map`, `std::unordered_map`, `std::array`, etc., ever.

## Error handling

- No exceptions (they're compiled out anyway).
- Expected failures: return the zero value (ZII), or a small result struct with
  the payload plus an `ok`/error field when the caller must distinguish causes:

```cpp
struct ParseU32Result { u32 value; b32 ok; };
```

- Programmer errors and invariants: `ASSERT(cond)` — a project macro that traps
  (`__builtin_trap`/`__debugbreak`); compiled out or kept in release as we decide
  later. Assert liberally.
- No error codes threaded through five layers; handle failure close to where it
  happens, or return zero and let ZII make the failure path safe to execute.
- Guards earn their place. A liveness/null check exists only where it owns a
  failure mode (LOG + recover), where the zero path would actually corrupt
  state (e.g. a write through the dummy slot), or as an `ASSERT` stating a
  contract. Everywhere else, trust ZII — the dummy path executes harmlessly;
  don't re-check what a callee already makes safe.

## Templates and operator overloading

Templates are a tool for **removing repetition on plain data**, not for
metaprogramming.

- OK: `Slice<T>`, `arena::allocate<T>`, `min`/`max`/`clamp`/`swap`, a generic
  arena-backed array. Simple, shallow, obvious at the call site.
- Not OK: SFINAE, `<type_traits>` gymnastics, CRTP, concepts beyond the simple
  contract-spelling kind sanctioned in *Language subset*, variadic template
  recursion, expression templates, template code whose instantiation you can't
  picture immediately.
- Operator overloading is for arithmetic value types only — `V2`, `V3`, `V4`,
  `Mat4`, `Rect`: `+ - * /`, plus `= default` comparisons (`==` or `<=>`).
  Never overload `->`, `()`, conversions, or any operator for "clever"
  non-arithmetic semantics. Decided exception: `String` has a custom value
  `operator==` (byte comparison, `!=` via the C++20 rewrite) — a view compares
  by contents, never by pointer. Two bare literals still compare as pointers;
  wrap one side in `String`. Decided exception: a view type whose range-for
  needs an iterator shim (`game::ChildrenView` — dense u16 slots resolved to
  `Thing*` on the fly) may define a nested `Iter` with `*`, `++`, `!=` —
  those three, nothing more, and only in service of `begin`/`end`.

## Naming and formatting

- Types: `CamelCase` — `Entity`, `RenderCommand`, `String` — living in their
  module's namespace (core types are global).
- Functions and variables: `snake_case`. The module namespace supplies the
  outer noun — `arena::push`, `renderer::flush` reads as `namespace::verb`.
  When a module owns several nouns, keep the inner noun as a prefix inside the
  namespace: `world::entity_spawn`, `world::province_at`. Overload sets on
  value types (`length(V2)`, `length(V3)`) are the exception.
- Namespaces: `snake_case`, named after the module file.
- Constants and macros: `UPPER_SNAKE_CASE` — `MAX_ENTITIES`, `ASSERT`.
- Enums: `enum class` with an explicit underlying type, `CamelCase` values —
  `UnitKind::Legion`. Cast explicitly (`(u32)kind`) when an integer is needed
  for indexing or flags.
- Pointers and references bind to the type: `Entity* entity`, `T& value` — not
  `Entity *entity`.
- Parameter order: the system/subject struct first (see *Fat structs*). An
  output arena — one the result is allocated into — leads the remaining
  parameters (`draw_map(Arena* arena, ...)`); a scratch arena trails them
  (`movement_day_begin(Game* game, Arena* scratch)`), so an arena's position
  says whether its memory outlives the call. One exception: a trailing
  callable parameter (comparator, body) comes after everything, scratch
  included — `sort::stable(slice, scratch, less)`.
- Braces on the same line, 4-space indent, no tabs. The module namespace block
  does not add an indentation level.
- Comments explain *why* and document non-obvious invariants (units, coordinate
  spaces, who owns what arena). No comment restates the code.

## Project layout and unity build

Code lives in `src/`, flat except for multi-file modules, which get one
directory (`src/ui/`) whose files nest inside the directory's namespace
(see *Namespaces*). A **module** is one self-sufficient `.hpp`
file: `#pragma once` at the top, then `#include`s of the modules it depends
on, then the module's namespace with its types, declarations, and
implementations. A **root** is a `.cpp` file, one per binary — always
directly in `src/` (the build scripts glob `src/*.cpp` non-recursively;
module subdirectories hold `.hpp` only). The extension
encodes the rule: `.hpp` is included, `.cpp` is compiled. (Modules are named
`.hpp` rather than `.cpp` because clang's include completion only offers
header-like extensions — with `.cpp` modules the LSP can't complete
`#include` lines.)

```
src/
  core.hpp     // global layer: primitive aliases, ASSERT, Array/Slice/String — no deps
  vmem.hpp     // vmem:: — virtual memory reserve/commit; platform blocks inside
  arena.hpp    // arena:: — includes core.hpp, vmem.hpp
  <system>.hpp // one module per system: world.hpp, renderer.hpp, assets.hpp, ...
  ui/          // ui:: — the UI module, one directory, nested namespaces:
               //   ui.hpp (ui:: — the facade: ui::Ui, load, run, Result),
               //   layout.hpp (ui::layout — the engine), style.hpp (ui::style),
               //   ir.hpp (ui::ir), data.hpp (ui::data), walk.hpp (ui::walk);
               //   games talk to ui.hpp + ui::data, the rest is machinery
  ray.hpp      // ray:: — raylib boundary, declaration-only (see *Boundary TUs*)
  ray.cpp      // BOUNDARY TU: the only file that includes raylib.h — not a root
  main.cpp     // ROOT: the game binary
  <tool>.cpp   // ROOT: one per extra binary (asset packer, test runner, ...)
```

- **Modules are included, never compiled.** Only root files are handed to the
  compiler. Each binary is exactly one root `.cpp` — its own single TU — that
  includes exactly the modules it needs. `#pragma once` makes repeated
  includes free.
- **Every module is include-safe and self-sufficient**: it includes its own
  dependencies and parses standalone. This is what lets clangd check each file
  in isolation, and what lets any new root pick an arbitrary subset of modules.
- **The include graph is a DAG.** One-way includes between systems are normal
  and expected (`world.hpp` includes `renderer.hpp` to emit render commands).
  A cycle is a design error — fix it by hoisting the shared type into the
  module that owns the contract (or into core), or by letting the root mediate
  with plain data. Never fix it with a forward-declaration interface file.
- Dependencies point downward: core at the bottom, then the platform modules,
  arena, systems, roots on top.

### Boundary TUs — third-party name isolation

raylib's header declares unprefixed C names (`Color`, `Rectangle`, `DrawText`,
the color macros, ...). Namespace-wrapping the include does not contain it —
macros ignore namespaces, and nested standard includes get captured — so such
a library gets a boundary pair, the one sanctioned break in the unity build:

- `ray.hpp` — an ordinary declaration-only module: the `ray::` API in project
  types, no raylib include, no implementations.
- `ray.cpp` — the only TU that includes `raylib.h`; implements `ray.hpp`,
  converting types and strings at the boundary. Not a root: `x.sh`
  compiles it once to `build/ray.o` and links it into every binary.
- Enums that mirror library constants (`ray::Key`) are `static_assert`ed
  against them inside the boundary TU.
- A boundary TU is a second TU linked into every binary, so any module it
  includes (directly or transitively — `core.hpp`, `math.hpp`, `pool.hpp`)
  now lives in two TUs per binary. The `fn` annotation (see *Language
  subset*) is what makes that safe — it is why every module function is
  `fn`, and why the boundary TU's own implementations of its module API are
  the one place `fn` must NOT appear.

This treatment is earned, not default: a header-only library with prefixed
names is just included; a boundary pair exists only where a header would
otherwise spill unprefixed names into our TUs.

### Platform layer

Platform-specific concerns (virtual memory, files, timing, windowing) are
ordinary modules. Inside, the shape is: the namespace's shared declarations
first — that is the contract — then one implementation block per platform,
guarded by platform flags:

```cpp
#pragma once
#include "core.hpp"

namespace vmem {

u8*  reserve(usize size);
void commit(u8* base, usize size);
void release(u8* base, usize size);

}

#if defined(__APPLE__)
#include <sys/mman.h>
namespace vmem {

u8* reserve(usize size) { /* mmap */ }
// ...

}
#elif defined(_WIN32)
// VirtualAlloc block
#else
#error "unsupported platform"
#endif
```

### Build

`x.sh` is the whole build system — one `clang++` invocation per root, no
Makefile, no CMake. A unity build has one compilation node per binary; there
is nothing to track incrementally. `x.bat` is its Windows twin — same
commands, same profiles, same clang — and the two must stay behaviourally in
sync: a change to one is a change to both. Windows-only deviations live in
`x.bat` with a comment (MSVC CRT defines, `.lib` link forwarding, raylib
compiled directly with clang instead of via its Makefile since there is no
`make`). `third_party.a` is platform-specific and never committed — each
machine builds its own via the `third_party` command.

- Commands: `clean`, `build`, `run` (rebuilds only when stale), `third_party`.
- Flags: `-std=c++20 -fno-exceptions -fno-rtti -Wall -Wextra -Werror`
  (unused names — variables, parameters, functions — warn without failing
  the build, so stubs keep compiling; `-Wno-reorder-init-list`
  because out-of-order designated init is house style — call sites order
  fields by meaning, not declaration). The default profile is
  `-O1` with `ASSERT`/`LOG` enabled; `--release` is `-O2` with both compiled
  out; `--debug` adds `-g` to either.
- The script also emits `compile_commands.json` with one entry per file in
  `src/` (modules and roots alike), same flags, so clangd diagnoses every
  module standalone.

### Third-party code

Third-party libraries never enter our TUs.

- Each library is compiled separately, using whatever build system it ships
  with; the results are archived into one `third_party.a` static library that
  is linked into the app.
- Our code includes third-party *headers* only (declarations, no
  implementation blocks).
- Header-only libraries (stb_*) get a small `.c` file and build script under
  `third_party/` that instantiate their `*_IMPLEMENTATION` blocks into the
  archive.
- `-Werror` applies to our code only; third-party code builds with whatever
  flags its own build chooses.

## Testing

Tests exist to **trip**, not to specify. `src/test.cpp` is the test root — one
binary holding dense tests for core and data modules (arena, vec, string,
tabula, and future foundational code). Gameplay and rendering are verified by
running the game, not by unit tests.

- **Tripwires, not specs — maximize coverage per line.** One dense test
  function exercises many behaviors at once; mixing concerns is encouraged.
  `CHECK` prints file:line, so a trip is still precise enough to diagnose.
- **Plain-data harness.** Tests are free functions `b32 test_x()` listed in a
  static `{name, function pointer}` table that `main` loops over. A failed
  `CHECK` prints and fails that test; the suite keeps going; the exit code is
  the failure count. No auto-registration, no filters, no framework.
- **Crashes kill the suite, by design.** The runner prints each test's name
  before running it, so an ASSERT trap or segfault names its culprit — a
  crash is a bug to fix now, not a result to tally.
- **Each test owns its arenas** — reserve/release locally, sized per test.
  There is no shared runner arena.
- `./x.sh build` compiles every root, so test code can never silently
  rot; `./x.sh test` rebuilds when stale, then runs the suite.
- **Invariants, never choreography** (decided 2026-07-22, after purging a
  crop of scripted-journey tests). Sim tests may assert contracts that
  survive any rules change — `validate() == 0`, orders terminate, a
  resolved choice closes the interaction, wire formats parse — never
  gameplay choreography (who meets whom on which day, what a prompt
  offers). Rules and tuning are content; playing the game is their test.
- The corpus test (`test_game_corpus`) loads the real `data/` files,
  demands zero problems, then plays blind — ordering the player somewhere
  and resolving every prompt sight unseen — with `validate() == 0` held
  every day. Editing `data/` must never break it unless the data is
  actually broken.

## When extending this document

New rules earn their place the same way code does: when a real situation comes
up that this document doesn't cover, decide it in conversation, then record the
decision here so it holds from then on.
