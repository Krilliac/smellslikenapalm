# Agent Session Bootstrap

Scope: entire repository (`smellslikenapalm` / RS2V Custom Server).

For every new session/chat in this repo:

1. Read [`CLAUDE.md`](CLAUDE.md) first.
2. Then skim the docs — start at [`docs/Home.md`](docs/Home.md) or
   [`docs/_Sidebar.md`](docs/_Sidebar.md) for the table of contents.
3. Pending and deferred work is tracked in [`TODO.md`](TODO.md).
4. Before touching netcode or the handshake, read the relevant reverse-engineering
   spec under `docs/` (`docs/RS2V_*`, `docs/UE3_*`). These encode hard-won,
   hard-to-re-derive knowledge about how the retail client behaves on the wire.
5. Use `CLAUDE.md` plus the relevant docs pages as persistent project context for
   workflow, conventions, and task execution.
6. If concurrent sessions are possible, follow [`CLAUDE_PARALLEL.md`](CLAUDE_PARALLEL.md)
   — run `tools/parallel/status.sh` and claim your subsystem before editing.

If `CLAUDE.md` is missing, report that clearly and continue with available context.

## Project summary for agents

RS2V Custom Server is a from-scratch, clean-room **dedicated-server emulator** for
*Rising Storm 2: Vietnam* (an Unreal Engine 3 game, `EngineVersion 7258`), written
in C++23. It reconstructs the server side of the UE3 control-channel protocol from
the retail client's observed wire behaviour, runs an authoritative game simulation,
and layers on an EAC *emulation*, telemetry, and an INI-driven config system.

Two rules dominate everything:

- **The client is untrusted; the server is authoritative.** Every wire read is
  bounds-checked, every client-asserted value is validated server-side before it
  touches game state. See `CLAUDE.md` → "Trust boundary."
- **The wire format is dictated by a client we do not control.** Conform to it; do
  not redesign it. Packet tags and message ordinals are an ABI.

It is **not** an official Tripwire/Epic product and contains no decompiled game
code — clean-room reconstruction only. See `CLAUDE.md` → "What is this?" for full
scope and non-goals.
