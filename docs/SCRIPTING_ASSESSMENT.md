# Scripting Assessment — keep C#, move to Lua, or rethink?

_Investigation requested 2026-06-26. Verdict: **rip out the dead C# engine; adopt Lua (via sol2)
behind the existing host API.**_

## 1. What exists today

`src/Scripting/` — **2347 LOC**, all currently **dead**:

| File | Role | State |
|---|---|---|
| `CSharpEngine.cpp/.h` | C# runtime host | **Broken** — uses deprecated **.NET COM hosting** (`mscorlib.tlb` / `ICorRuntimeHost`); the non-Windows path is a pure stub returning `false`. |
| `ScriptManager.cpp/.h` | event dispatch + engine glue | Hardcodes C# (`#import "mscorlib.tlb"`). Not engine-agnostic. |
| `ScriptHost.cpp/.h` | the host API exposed to scripts | **Good design** (see §2); impl tied to the C# host. |
| `ICSharpScriptHost.h` | C#-side interface | C#-specific. |

Hard facts:
- **Disabled by default** — `option(ENABLE_SCRIPTING … OFF)`; the CMake comment says it "uses
  deprecated .NET COM hosting … and does not currently build."
- **Not wired in** — nothing outside `src/Scripting/` instantiates `ScriptManager`/`ScriptHost`
  (grep is empty). Zero call sites in `main`, `GameServer`, anywhere.
- **Origin** (git): "Implement Basic C# Scripting" → "Cont. Scripting support, Better EAC Memory
  Writing/Reading/Allocating" — an early experiment entangled with EAC memory poking, not a
  maintained server-extensibility layer.

**Conclusion:** it is not "scripting we have," it is "scripting we attempted and abandoned." There is
nothing to preserve at the engine level.

## 2. What scripting is actually FOR here

The one genuinely valuable artifact is the **host API surface** in `ScriptHost.h` + the event list in
`ScriptManager.h` — it defines exactly the right extensibility model for a game server:

- **Event hooks:** `OnServerStart/Shutdown`, `OnPlayerJoin/Leave`, `OnChatMessage`,
  `OnMatchStart/End`, `OnPlayerMove/Action/Kill`.
- **Actions:** `BroadcastChat`, `SendChatToPlayer`, `KickPlayer`, `BanPlayer`, `UnbanPlayer`,
  logging, scheduled callbacks.

This is the classic **server-mutator / game-mode / admin-automation** surface — the same thing WoW
emulators expose to Lua via **Eluna** (MaNGOS/TrinityCore). It is **language-agnostic**: keep the
API, swap the engine underneath.

Note: this is NOT for running the game's real UnrealScript mutators (we have no UnrealScript VM and
never will). It is for **our** server's extensibility.

## 3. Options

| Option | Embed cost | Runtime weight | Modder friendliness | Hot reload | Fit |
|---|---|---|---|---|---|
| **Rework C# onto CoreCLR** (`hostfxr`/`nethost`) | High | **Heavy** (~50–200 MB .NET runtime) | Medium | No | ✗ overkill, deploy friction |
| **Lua (sol2 / LuaJIT)** | **Low** | **Tiny** (~300 KB) | **High** (the game-modding lingua franca) | **Yes** | ✓✓✓ |
| **AngelScript** | Low–Med | Small | Medium (C++-like, typed) | Partial | ✓ (typed alternative) |
| **Native C++ plugins** (`.dll/.so`) | Low | None | Low (needs a compiler) | No | ◑ good for perf-critical add-ons, bad for casual scripting |
| **No scripting** (config + C++) | — | — | — | — | ◑ fine short-term, caps community modding |

## 4. Recommendation — **Lua via sol2**

1. **Delete** `CSharpEngine.*`, `ICSharpScriptHost.h`, and the COM guts of `ScriptManager` — 2300+
   lines of dead, non-building, deprecated code. (It's gated OFF, so removal is risk-free.)
2. **Keep the API model** from `ScriptHost.h` / `ScriptManager.h` (events + actions) — it's the
   right design. Re-express it as Lua bindings.
3. **Add a `LuaEngine`** (sol2 — header-only C++17 binding over Lua 5.4; optionally LuaJIT for
   speed). Bind the action API; dispatch the events into Lua callbacks. Sandboxed env per script,
   hot-reload on file change.

Why Lua:
- **Precedent the project already gravitates to** — the MaNGOS WoW-emulator reference the user has
  been using ships Eluna/Lua for exactly this. Server admins/modders expect Lua.
- **Lightweight + trivial to embed** — no giant runtime, no COM/hosting fragility, builds everywhere
  the server does (the thing that killed the C# attempt).
- **Maps cleanly** onto the existing event/action surface; bindings are a few hundred lines with
  sol2, not a runtime-hosting saga.
- **Hot-reloadable + sandboxable** — iterate server logic without recompiling; cap what scripts can
  touch.

If static typing is wanted later, **AngelScript** is the fallback (C++-like, type-safe) — but Lua's
ecosystem and modder familiarity win for a community server.

## 5. Priority

**Not now.** The live priority is gameplay (pawn spawn → in-world). Scripting is a post-gameplay
extensibility feature. The concrete action that *is* worth doing soon is **deleting the dead C#
subsystem** so it stops implying a capability we don't have; the Lua build-out comes after the client
can spawn and play.
