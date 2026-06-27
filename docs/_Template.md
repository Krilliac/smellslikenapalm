# <Page Title>

> **Audience:** <Server operators | Netcode/protocol devs | Game-sim devs | Security | QA | Mixed>
>
> **Execution context:** <Server main loop | Network thread | Worker pool | Build-time tool | Offline>
>
> **Maturity:** <v0 (just landed) | active | stable | deprecated>

## Overview

Explain what this subsystem, protocol, or guide covers and why it exists. One
paragraph.

## When to Use / When to Read

List concrete scenarios where this page is the right reference.

## Trust & Validation Surface

If this subsystem processes client-supplied input, state the trust boundary: what
arrives untrusted, where it is bounds-checked / validated, and what fails closed.
Link to [`SECURITY.md`](SECURITY.md). Subsystems with no client input say "none."

## Threading & Concurrency Model

Document which operations run on the main loop vs. the network thread vs. a worker
pool, which locks are held, and any ordering/`std::atomic` requirements. Note the
locks each component owns.

## Key APIs and Types

Provide key classes, structs, and entry points with short purpose notes. Reference
exact paths (e.g. `src/Network/BitReader.h`, `src/Game/RoundManager.h`).

## Wire / Config Surface (if applicable)

For protocol pages: message ordinals, bunch/bit layout, byte order, and which RE
note (`docs/re/…`) it derives from. For config pages: the INI keys, defaults, and
validation rules.

## Performance Notes

Call out hot paths (per-tick / per-packet), allocation behaviour, and scaling
characteristics under player load.

## Known Limits / GAPs / STUBs

Itemise what is **not** implemented yet. Anything carrying a `// STUB:` or
`// GAP:` marker in the source belongs here so future audits can find it from the
docs side too. See [`../CLAUDE.md`](../CLAUDE.md) for the marker convention.

## Troubleshooting

Common failure modes and direct fixes.

## Related Pages

Link to adjacent docs pages, specs, and RE notes.
