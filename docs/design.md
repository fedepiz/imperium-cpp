# imperium — Simulation design

Decision record for the game state / World lifecycle. Decided 2026-07-19 in
conversation; the reasoning is captured so future changes argue against the
*reason*, not just the rule. Inspirations: Handmade Hero (ids as persistent
truth, pointers as frame-local currency), Dreams (flat arrays of PODs,
whole-state buffering), Romance of the Three Kingdoms X (time flows when the
player lets it).

## The World

- `game::World` is the authoritative game state: one flat POD struct, ZII
  (the all-zero world is valid and empty), no pointers inside — ever. This is
  load-bearing for saves, snapshots, and the buffering scheme below.
- There is exactly **one** World. Nothing else is authoritative: presentation
  state (camera, hover, animation timers, interpolation) lives outside World,
  on the app/render side, and is rebuilt or smoothed from World — never the
  reverse.
- Things are referenced by generational `Id` (`u16 slot`, `u16 generation`,
  odd generation = alive). Slot 0 is the permanently-zeroed dummy; failed
  lookups return it, so the null path is safe to execute. Pointers to Things
  are frame-local currency only — they never outlive the phase that looked
  them up.
- Width limits, considered and accepted: `u16 slot` caps things at 65534;
  `u16 generation` wraps after ~32k respawns of one slot, so a stale handle
  could in principle collide after a wrap. The odds are negligible at this
  game's scale; revisit only if profiling of *bugs* says otherwise.

## The tick

- The tick quantum is **one day**. Sub-day time does not exist; anything that
  spans days (travel, battles, construction) is a multi-tick process.
- The pump is always on: every render frame the app calls
  `game::tick(World*, Slice<TickCommand>)`. Commands are drained every tick,
  unconditionally.
- Time advancement is a command: `AdvanceTime{elapsed}` carries how much
  wall time passed, pre-scaled by the time controls. `elapsed` is an
  **integer** (no floats in command payloads — commands are the replay log,
  and integer payloads replay exactly).
- The **sim** decides how many day-ticks that elapsed time becomes — zero
  when time is blocked. The blocking reasons (a modal event awaiting
  response, a mode where time holds) are authoritative World state, not UI
  state. "Paused" is not an app concept; it is the sim electing to convert
  elapsed time into zero days. Orders therefore take effect immediately even
  while time is held.
- The partial-day accumulator lives in World, so saves capture progress
  toward the next day.

## The phase invariant

This is the one rule that everything else leans on:

> **Parallel phases never mutate World.** They read `const World*` and write
> only private outputs. All World mutation is serial. Joins merge private
> outputs in canonical order (slot order, job-index order) — never
> completion order.

- A tick is a flat, top-to-bottom-readable sequence inside one function:
  drain commands → serial prep → parallel gather → merge/resolve →
  despawn flush → surface interrupts to the app. It must stay a flat
  sequence, not grow into a task graph — the legibility is the feature.
- "Entity-parallel vs system-parallel" is a non-question under the
  invariant: what a gather pass fans out over (things, provinces, chunks,
  several distinct analysis passes at once) is a per-case scheduling choice
  with no safety implications, because read-only + private output is
  race-free regardless of decomposition.
- Cross-thing interaction never happens inside a gather: a gather either
  reads last-tick state (one-tick latency is a *defined semantic*, not a
  bug) or emits a request for the resolve phase. Resolve must stay
  O(interactions), never O(things) — it is the serial bottleneck.

## Buffering

- World stays the single copy. The gather phase's write target is a
  transient next-things scratch array owned by tick machinery **outside**
  World: each job copies its own slot from `const World`, mutates the copy
  (dead slots get a plain copy — the baseline copy rides inside the jobs),
  then a serial bulk memcpy over `[0, high_water]` lands it back.
- No selector, no second buffer in saves, no `things[2]`. The extra bulk
  memcpy per tick is the accepted price (~1–2 ms even at fat Things; fine at
  a handful of ticks per frame).

## Spawn / despawn

- `spawn`, `mark_for_despawn`, `despawn_flush` are **serial-phase-only**
  APIs (prep, resolve, out-of-tick). Gather passes emit per-job POD
  requests (spawn payloads, despawn ids) merged in job-index order during
  resolve.
- `mark_for_despawn` parks the thing on the despawn list but leaves it fully
  alive — handles resolve until `despawn_flush`, which runs once at end of
  tick: generation bumps odd→even (all outstanding handles die at this
  single point) and slots return to the free list. The `marked_for_despawn`
  flag exists to make marking idempotent (double-marking an intrusive list
  would corrupt it) and doubles as an "is this dying?" query during the
  tick.
- Fresh slots come from a high-water mark, recycled slots from the free
  list; a zeroed World needs no initialization pass. A full world refuses
  the spawn with a one-shot LOG and returns the nil id — degrade and log,
  no eviction.
- A thing spawned in resolve is rendered this tick and first simulated next
  tick. A thing marked in this tick's resolve is dead to handles at flush,
  this same tick.

## Mutation contexts (complete list)

1. **In-tick serial** (prep, resolve): full World access, all APIs.
2. **In-tick gather**: `const World*` + private outputs only.
3. **Out-of-tick exclusive** (load, save, new-game init): same rights as
   resolve, just outside the loop. New game = zeroed World + serial setup
   writes.
4. **App/UI/render**: read-only, between ticks. Player intent enters
   exclusively as commands — there is no other path into World.

The command queue itself is app-owned (outside World); `tick`'s signature
names its two inputs. Accepted consequence: commands pending at save time
are lost — kept rare by the always-on pump draining every frame.

## The player, orders, and actions

Decided 2026-07-22. The game is ROTK X-shaped: the player IS one Thing
(`is_player`), not a god-view commander. The map is a viewer and a *target
picker* — the player only ever commands themself, and all intent enters as
commands (mutation context 4). Click a cell → a panel lists the visible
things standing there → picking one issues `travel_to = <packed id>` (an
integer payload — commands are the replay log). Clicking empty ground
selects nothing; orders target things, never cells, until a real need says
otherwise.

- **One flat Action vocabulary** (`game::Action`: Nil, Meet, Enter, ...):
  the game's verbs. Interaction choices resolve to an Action; orders carry
  one. New doorways into the vocabulary don't grow new enums.
- **Orders are "move to X, on arrival do Y"**: `Thing::move_dest` +
  `Thing::move_action`. NPCs execute Y serially in the tick's
  event-consumption loop (the Arrival event carries the action, because the
  step already cleared the order). Y = Nil means arrive and stand there.
- **The player is prompted instead**: a collision — a Meet, or arrival at
  the ordered destination — raises an Interaction whose choices are derived
  from the target's affordances (`BodyKind::enterable` → Enter; a person →
  a greeting). The Interaction records subject and target so a resolved
  choice knows who acts on what. First prompt wins the tick; an open
  interaction is never overwritten (later events still log), and the day
  loop stops advancing once one opens — remaining requested days are
  dropped, not queued.
- Meeting a town IS the entry prompt: stepping onto its cell fires the Meet,
  whose prompt offers Enter. A prompt declined leaves you standing on the
  cell; re-targeting the town completes as a zero-length arrival, which
  still emits the Meet (see *Data-driven interactions*) and re-opens the
  prompt.
- Camera follows the player's map anchor (presentation only); manual pan
  breaks into free look; one key re-engages follow. ZII: the zero camera
  follows.
- An open travel picker holds time exactly like an interaction, but from the
  app side: the pump feeds zero elapsed while the panel shows. Both are
  decisions the world waits on; commands still drain.

## Data-driven interactions

Decided 2026-07-23. **Data names things, code computes things.**
`data/interactions.txt` is a sea of fragment records compiled at load into
typed defs living on Game (content, like BODY_KINDS — never in World: saves
carry no content, replays run against edited files).

    text = {
        trigger = { target_kind = town }
        text = "It is a town"
    }
    choice = {
        trigger = { target_kind = town }
        text = "Enter"
        action = enter
    }

- A trigger is a flat record of facts the sim computed: every named field
  must equal its fact (AND); an absent/zero field matches anything. No
  expressions, no boolean operators — ever. New expressiveness means new
  code-computed fact fields, not a query language.
- **Fragments compose instead of bundling** (revised the same day from a
  first-match bundle design: description verb x target affordance forced
  N x M bundles). Every matching `text` record concatenates into the
  interaction's description (file order, space-joined); every matching
  `choice` record is offered (file order). Orthogonal content dimensions
  stay N + M records.
- The interaction's title is the target's name — computed, not authored. It
  never varied in practice; a title record kind can return if content
  earns it.
- Choices name Actions from the one flat vocabulary (action_from_name);
  templates interpolate a closed, code-provided var set ($TARGET, $SUBJECT)
  at fire time, appended straight into the World's Interaction.
- An interaction starts only when at least one text AND at least one
  choice instance composed; anything else is a LOGged content gap, every
  occurrence — a choiceless interaction could never be resolved, and
  one-shot suppression state would have to live in World.
- Unknown actions, kinds, and fields are load problems, and a record whose
  trigger had one is dropped whole — a typo must not silently widen a
  fragment onto every interaction. R hot-reloads the file alongside the UI.
- **Meets are the only interaction source** (decided 2026-07-23, retiring
  a brief `event = meet|arrival` trigger fact). A Meet is the social fact
  — two things colliding, symmetric; an Arrival is order lifecycle (clear
  the order, run NPC carried intent) and starts nothing by itself.
  Interactions are written from the player's POV: the subject is always
  the player and $TARGET the other party, whichever side stepped into
  whom. Meets never start interactions mid-event-consumption — events
  mutate freely, so the player's meetings QUEUE (several may, in one day)
  and start against the settled world; the first to open wins the tick.
- **A zero-length arrival emits a Meet**: an order deliberately engages
  its goal, so completing it is contact even when the pair were already
  co-located (the day-start companion rule would otherwise suppress it,
  stranding a player who declined a town's interaction outside forever). The
  rule that falls out: every completed order ends in a Meet with its goal
  anchor, or cancels.
- **`scope` names a code-computed set** (decided 2026-07-23 — the UI's
  list-template pattern: data declares a template, code supplies the
  rows). `scope = occupants` expands a fragment into one instance per
  thing contained in the interaction's target, in slot order (deterministic,
  replay-safe). An instance's $TARGET and its choice's action target are
  that element — "Greet $TARGET" per resident. Iteration lives in code;
  data names the set. Scoped records over an empty set contribute
  nothing; the start gate counts instances, not records.
- Runtime choices carry their own action target, filled at composition
  (the interaction's target for plain records, the element for scoped
  ones). Action::Meet is the engagement verb: the player's Meet queues an
  interaction with the action's target, started at the same settle point
  as event meetings — command-driven, so it rides the replay log with no
  synthetic events. This is also the conversation-opening seam.

## Saves

- Snapshot format: `{magic, version, sizeof(World)}` header + the World
  bytes verbatim. Load = validate header, read into a zeroed World, done —
  no fixup pass, because World contains no pointers.
- Version bumps on any World layout change; old saves are rejected during
  development. A field-serialized format can replace the payload behind the
  same header when layouts stabilize.
- The same memcpy serves in-memory snapshots (autosave, undo experiments,
  replay checkpoints).

## Determinism

- Guarantee: **same binary + same starting World + same command log ⇒
  bit-identical World**, single-threaded or threaded. This is what makes
  replay-based debugging and desync tests real.
- Cross-build/cross-platform bit determinism is explicitly *not* a goal (no
  lockstep multiplayer planned); floats in sim code are fine.
- The habit that keeps it true: sim code never reads anything unlogged — no
  wall clock, no render state, no uninitialized padding, and canonical-order
  joins so thread scheduling stays invisible.

## Threading

- Shape now, threads later: gather passes are written against a
  `job::par_for`-style seam from day one, executed by a plain serial loop
  until a profile shows a tick over budget. The disciplines (no World writes
  in gather, canonical merges) are real and reviewable immediately; the
  worker pool drops in behind the same seam without touching gameplay code.
  `<atomic>`/OS primitives enter the codebase then, not before.
