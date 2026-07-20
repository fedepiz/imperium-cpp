# Going deeper

## Theory (the guess is the spec)
The model's prior is a compression of how APIs, configs, and CLIs are conventionally
shaped, so what it reaches for is the best cheap proxy for what feels familiar to a
developer. Familiar is what gets adopted. You normally only hear from the few users
who push through an unfamiliar design; an LLM shows you what people reach for cheaply
and at volume (it "fails" thousands of times a day). The move is to stop fighting the
guess and make your API whatever the prior reaches for. Guesses are systematic and
reproducible (see the slopsquatting research), so a convergent guess is a real
signal, not noise. Caveat: capable agents are "too competent" and can reason around
an unfamiliar design from first principles, so withhold the surface and capture the
first instinct before exploration.

## Modes
- **Cold** (no read-set): what they reach for with no help. The purest read on what is familiar.
- **Doc-informed** (read-set provided): whether your docs make the right shape familiar. Hand it a README or the docs site (the orchestrator fetches a URL and inlines its content; the agent never opens links itself); the agent reads it then guesses.
- **Doc-swap converge loop**: when you choose the teach fallback instead of conforming, this checks the teaching worked. README v1 produces a divergent guess, change one sentence, re-run with README v2, get the conforming guess. The implementation is never touched.

## When NOT to conform (the fallback)
Conforming is the default, not a law. Keep your API and teach instead when the
guessed shape is unsafe, violates a hard constraint, or would break a real
invariant. Counter-example: a framework where a reachability check must not perform
I/O. Agents may confidently reach for an I/O-in-the-read shape, but adopting it would
break the contract, so the move is to keep the API and make the unfamiliar constraint
loud in the docs (and use the doc-swap loop to confirm the teaching lands). A real
instance of this fallback was a connection worker in one of our systems: agents put the network
dial in the wrong place, and the fix was a one-sentence README change, not an API
change. Treat the fallback as the exception you justify, not the reflex.

## Deferred (not in v1)
- **Diagnostic ladder**: same task at 3 context tiers (blind / signatures-only / full-docs) to localize whether the gap is naming, shape, or docs.
- **Multi-model / persona diversity**: junior/senior/other-ecosystem agents.
- **Live UI driving (Playwright)**: drive the real product instead of reading component source for the answer key.
- **Auto-applying conform moves**: the skill only ever recommends; it never edits the API under test.
