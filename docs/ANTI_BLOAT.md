# Anti-Bloat Guidelines

> **Audience:** Anyone adding code
>
> **Maturity:** active

AI-assisted development has a structural bias toward complexity: adding features
"just in case," creating helpers for single uses, over-engineering simple problems,
and building systems that are never wired in. In a netcode-heavy server — where a
wrong abstraction calcifies into the wire protocol or the replication layout, and a
client you do not control depends on the exact bytes — this bias is **more**
dangerous than in ordinary application code.

The goal is **sanity, not sacrifice.** Keep code clean without stripping legitimate
verbosity or readability. A clean 450-line `.cpp` is fine; a cryptic 200-line one
is not.

## Sensible thresholds (not hard limits)

These are guidelines for **when to pause and think**, not absolute rules.

| Thing | Threshold | What to do |
|-------|-----------|------------|
| `.cpp` file size | ~500 lines | Split if it's doing multiple jobs; leave if it's one coherent unit |
| `.h` file size | ~300 lines | Split unrelated types; data-heavy headers (tables, enums) are fine |
| Public methods per class | ~15 | Ask: does each method earn its place? |
| Function length | ~60 lines | Split on nested branching; a clear linear flow is fine long |
| Parallel subsystems doing the same job | 0 | Remove the duplicate |
| Per-packet allocations on the hot path | minimise | Reuse buffers/pools; allocation in the receive loop is a smell |

## The readability principle

**Never sacrifice readability to hit a line count.** Keep comments that explain
"why," use descriptive variable names (`bunchHeaderBits` > `bhb`), keep vertical
whitespace between logical sections, brace non-trivial bodies, and write one
statement per line. The question is always: *does this make sense to someone reading
it for the first time while a live server is misbehaving?*

## Before writing code — checklist

1. **Does this already exist?** Search first — especially for primitives: buffer
   readers, byte-swap, string/hex helpers, config accessors, the thread/memory
   pools in `src/Utils/`.
2. **Will this be called?** If you can't name the caller, don't write it.
3. **Can existing code do this with a small change?** Prefer editing over adding.
4. **Is this a one-time use?** Inline it — no helper function, no new class.
5. **Am I future-proofing?** Stop. Write only what today needs.
6. **Adding a new packet type / message ordinal?** The wire format is an ABI shared
   with a retail client you can't patch. Once a tag is on the wire, it's forever.
   Be sure, and document it in the relevant protocol spec.
7. **Adding a config key?** Give it a default and a validator, and document it in
   [`CONFIGURATION.md`](CONFIGURATION.md). An undocumented knob is a support burden.
8. **Is the code dead?** Delete it — don't comment it out; git history exists.
9. **Is a system built but not wired in?** Either wire it in or delete it. A
   subsystem that's never initialised or dispatched is worse than not existing — it
   rots until a refactor accidentally re-enables it. (See `CLAUDE.md` → Wiring
   Things In.)

## The interaction with "Fix Anything You Surface"

Anti-bloat says *don't add beyond the task.* "Fix anything you surface" says *fix
everything that's concretely broken right now, even outside your task.* These do not
conflict: the override is **only** for things that are broken **now** — a failing
test, a warning, a crash, a wrong value, a stale comment. It is **not** a licence
for speculative refactoring or "while I'm here" rewrites. Those remain anti-bloat
targets. See `CLAUDE.md` for the full statement of both rules.
