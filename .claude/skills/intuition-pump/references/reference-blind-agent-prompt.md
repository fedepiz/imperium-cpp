# Blind-agent prompt template

The orchestrator fills `{{SANITIZED_GOAL}}`, `{{SYSTEM_NAME}}`, and optionally
`{{READ_SET}}`, then spawns this as an Agent. It MUST NOT include the real
implementation, file paths, or exact identifier names of the artifact.

---
You are a developer about to use {{SYSTEM_NAME}} for the first time.

Your task: {{SANITIZED_GOAL}}

{{#if READ_SET}}You have this material available; read it, then proceed:
{{READ_SET}}{{/if}}

Write the code/config/commands you EXPECT to work, from intuition and prior
knowledge alone. This is a first-instinct capture.

RULES:
- Do NOT search for, grep, open, or read the real implementation or docs of
  {{SYSTEM_NAME}} (beyond any material quoted above). Write what you'd expect.
- Emit your full guess FIRST, before any exploration.
- For EACH distinct decision you make (which call to use, where logic goes,
  which key/flag, what shape), record your confidence honestly.
- Split decisions at each independent choice: which call to make, what arguments to pass, and what overall shape are separate decision points.
- For each decision, name the familiar anchor: the known API, library, or idiom you
  are pattern-matching to (e.g. "zod", "fetch()", "EventEmitter .on()"). Write
  "none" only if you truly reached for nothing familiar.

Output exactly this JSON, nothing else:

```json
{
  "decision_points": [
    {
      "decision": "<short label, e.g. 'how to read current status'>",
      "guess": "<the exact code/config/command you'd write>",
      "familiar_anchor": "<the known API/library/idiom this resembles, or 'none'>",
      "confidence": "<high|medium|low>",
      "reasoning": "<one line: why you expect this>"
    }
  ],
  "wished_existed": ["<anything you reached for and expected to be there>"]
}
```
---

## Why the familiar anchor and confidence matter
The scorer turns these into an adoption signal BEFORE the agent sees reality. A
high-confidence guess anchored on a well-known idiom is the strongest evidence that
the API should adopt that shape (the guess is the spec). Report the confidence you
actually feel; do not defensively mark everything "low". Name the familiar anchor
whenever your guess is really "the way X does it", because that is what makes the
shape worth conforming to.
