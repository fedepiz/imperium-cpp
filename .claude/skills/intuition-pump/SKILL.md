---
name: intuition-probe
description: Use when you want to know what shape a developer or user reaches for when they first meet your API, config format, CLI, or UI, so you can make your design that shape. Spawns a blind agent that commits the design it EXPECTS before seeing the real thing, then reports the shape they reached for and conform-first recommendations (make the API whatever the guess is). Triggers on "is this intuitive", "would a developer guess this", "intuition probe", "blind-test this API/config/CLI", "test the DX/UX of", "what would someone expect here", "make the API whatever the LLM guesses".
---

# Intuition Probe

Discover the shape a developer (or an LLM) reaches for when they meet your API,
config, CLI, or UI for the first time, then make your design that shape. An agent
guesses how to use the artifact blind, before it sees the real thing. The guess is
not an error to grade; it is a design proposal. Where the guess diverges from your
API, the default move is to conform the API to the guess, because the guess is what
feels familiar, and familiar is what gets adopted.

This follows the Jazz principle "make the API whatever the LLM guesses": you only
hear from the few users who push through an unfamiliar design, but an LLM shows you
what people reach for cheaply and at volume. See `references/going-deeper.md` for
the theory, the conform-first default, and when NOT to conform.

## What it measures (and the honest limit)
It surfaces what the model's prior reaches for, the best cheap proxy for what is
familiar to a developer. **At the default N=1 it surfaces CANDIDATES, it does not
CONFIRM:** a confident divergent guess is a candidate shape to conform to, but a
single draw cannot prove it is *the* familiar default, and a single match does not
prove your API is familiar. Dial N up; only convergence across agents confirms a
shape worth conforming to.

## Procedure

Step order matters: **freeze prompt (3) before reading the artifact (4) before
spawning agents (5)**. Reversing 3 and 4 contaminates the blind test.

### 1. Gather inputs
Ask the user for: the **artifact under test** (a file path, package name, CLI
command, or URL -- e.g. `plugins/foo/SKILL.md` or `git commit`), a **rough
goal**, an optional **read-set** (a README, a docs URL, or a snippet), and **N**
(default 1). If the artifact is a path/package/command, note it; do not read it
yet.

### 2. Sanitize the goal
Restate the rough goal as a pure outcome. Strip any leaked identifiers: method
names, config keys, flags, exact labels. Show the user:
> Sanitized goal: "<cleaned goal>" is that the task? (y / edit)

Wait for confirmation. The read-set is NEVER sanitized (a README is allowed to
contain the answer; that's the doc-informed test).

### 3. Freeze the blind prompt (BEFORE reading the answer key)
Take the template in `references/blind-agent-prompt.md` and fill its placeholders
(sanitized goal, system name, read-set if any) into a frozen working prompt that
you hold in your own context. Do NOT edit the shipped template file. Also record N
and the mode (cold / doc-informed) next to the frozen prompt so step 8 has them.
**Freeze the prompt now and do not revise it after step 4**, so the real
implementation cannot leak back into it. If the read-set is a URL, you (the
orchestrator) fetch it now and inline its content into the frozen prompt; the
blind agent is forbidden from opening URLs, so it must receive the doc text,
never a bare link.

### 4. Read the answer key
Read the real surface from the repo: source/signatures (API), schema/examples
(config), `--help`/command defs (CLI), component/route source (UI). If you cannot
read it, ask the user to paste the real surface. Keep it private; it never goes
into the blind prompt.

### 5. Run the blind agent(s)
Spawn N agents via the Agent tool, each with the frozen prompt text from step 3
as its prompt. Use
`subagent_type: general-purpose`. Collect each agent's `decision_points` JSON.
(For N=1, one call. For N>1, issue the Agent calls in parallel.)

### 6. Score
For each agent's guess, apply the `references/scorer-rubric.md` classification
against the answer key, either inline (you, the orchestrator) or by spawning a
scorer Agent. Produce the `findings` JSON. Each finding names the familiar anchor
the agent reached for and a conform-first recommendation; the adoption signal is
read from the agent's recorded confidence (and, at N>1, convergence). Do not invent
signal strength from hindsight. If you spawn a separate scorer Agent rather than
scoring inline, pass it three things: the agent's `decision_points` JSON, the answer
key from step 4, and `references/scorer-rubric.md`.

### 7. (N>1) Cluster + cross-validate
Group findings by decision point across agents. Mark a finding **priority** only
if at least 2 independent agents hit it. Single-agent findings are listed,
de-prioritized.

### 8. Report
Render per `references/report-format.md`: lead with the shape the prior reached for
(the candidate spec) and a conform-first recommendation per divergence. If N is 1,
include the candidate banner and never report the design as a confirmed familiar
default. Offer to save the report to a scratch/artifacts folder. **Never write to the
repo under test.**
