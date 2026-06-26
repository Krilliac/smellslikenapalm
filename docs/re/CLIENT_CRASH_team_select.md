# Client crash on team-select advance — analysis (VNGame.exe)

Dump: `C:\Users\natew\AppData\Local\CrashDumps\VNGame.exe.23920.dmp` (WER minidump, x64).
Parsed with python `minidump` (no cdb/windbg installed). Script: scratchpad/dump3.py.

## Crash
- **EXCEPTION_ACCESS_VIOLATION (0xC0000005)**, `ExceptionInformation = [0, 0]` = a **READ of
  address 0x0** (NULL-pointer dereference).
- **RIP = VNGame.exe+0xbbf712** (module base 0x7ff6b8c60000). The faulting code is inside the
  client's own UE3 engine (no symbols; VNGame.exe is the retail client).
- Crash-thread registers: `Rdx=0, R8=0, Rsi=0, Rbp=0` (several NULLs).

## Call stack (return addresses, all VNGame.exe + offset)
```
crash    VNGame.exe+0xbbf712      (native fn, NULL read)
caller   VNGame.exe+0xbc032a      (same ~0xbc0000 region)
         VNGame.exe+0x9431e7
         VNGame.exe+0x94e8b0
         VNGame.exe+0x11d6580
         VNGame.exe+0x8d2b38
         VNGame.exe+0x953a39
         VNGame.exe+0x93e5d8
         VNGame.exe+0x91c6aa
         VNGame.exe+0x981470
         VNGame.exe+0x11e4cc0
         VNGame.exe+0x151b460
         VNGame.exe+0x158ba80     <-- appears repeatedly = UObject::ProcessEvent / script call trampoline
```
The repeated `+0x158ba80` frames = the UE3 **script execution** path (ProcessEvent dispatching
UnrealScript). So the crash is a native function invoked FROM UnrealScript dereferencing null.

## Root cause
Our server replies to `SelectTeam` with `ChangedTeams` (handle 172). On the client that runs
`ROPlayerController.ShowRoleSelectScene` (ROPlayerController.uc:5919), which touches game state
we do NOT replicate with real values: `WorldInfo.GRI.Teams[TeamIndex]` (needs populated
ROTeamInfo), `ROMapInfo.NorthernSquads/SouthernSquads` (role/squad defs), `ROGameReplicationInfo
(WorldInfo.GRI).MaxPlayers` (cast). One of these resolves NULL and a native call derefs it -> AV.

The iteration-9 per-param "Send"-bit fix is what *exposed* this: it made `ChangedTeams` correctly
DECODABLE, so the client actually ran it (and crashed). Before, the misaligned bunch was silently
ignored -> no advance, no crash.

## Fix applied (superseded — see "Real fix" below)
First mitigation: on `SelectTeam`, record the team server-side (TeamManager::AddPlayerToTeam) but
DO NOT send the client-side advance. Stopped the crash; team menu stays up; no advance.

## Real fix (role-select advance, re-enabled)
RE (3-agent fleet + reading `ROPlayerController.uc:3533 ChangedTeams` directly) showed the earlier
"unreplicated state" diagnosis was incomplete. The actual cause:
- `GRI.GameClass` is NOT null — we replay the real server's ch54 GRI open verbatim, which carries
  `GameClass` (h33) = static class index **69601** = `ROGame.ROGameInfoTerritories`.
- The client BINDS `PRI.Team` itself at `ChangedTeams` line 3618 (`= GRI.Teams[TeamIndex]`), and our
  TeamInfo opens (ch21/56/76) already populate `GRI.Teams[]`. So no PRI.Team delta is needed.
- The crash was: we sent `ChangedTeams` with its **`GameTypeClass` param defaulted to none**. The
  client uses that param directly at line 3627 `ShowRoleSelectScene(GameTypeClass,...)`; with none it
  SKIPS `InitSquadsForGametype` (5941), squads stay empty, `GetSelectedRoleInfoClass()` returns none,
  and the role UI does `RoleClass.default.X` on a null class-default object → AV read 0x0.

Fix (ConnectionManager.cpp SelectTeam handler): send `ChangedTeams` (handle 172) with
`bShowRoleSelection=true` and `GameTypeClass` = the static class ref **69601** (the exact index the
client already accepted in GRI, so it resolves). The client then builds squads locally from its own
loaded `ROMapInfo` (map data is NOT replicated — `ROMI = ROMapInfo(WorldInfo.GetMapInfo())`). Param
bit-layout validated against UE3-src `UnScript.cpp:2980-3010`; wire bytes proven well-formed by
`tests/ActorReplicationTests.cpp::ChangedTeamsRpcBodyRoundTrips`. Index recovery + corroboration:
`docs/re/open_bunch_structure.md:185`, `docs/re/gri_ch54_properties.md`.

## To investigate further in the client (optional)
Load `VNGame.exe` in Ghidra/IDA/x64dbg at image base, go to offsets above. `+0xbbf712` is the
crashing native; `+0x158ba80` is (almost certainly) `UObject::exec<native>` / `ProcessEvent`.
Walking `+0xbc032a` -> `+0x9431e7` -> ... maps the ROPlayerController script path that fired it.
