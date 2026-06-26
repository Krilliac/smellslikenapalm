# PlayerController channel (ch2) S2C timeline, immediately post-Join

Source capture: `D:\RE-Tools\rs2_realserver_capture.pcapng`
Wire filter (S2C): `udp.srcport==7777 && udp.dstport==57867`
Decoder: `tools/mock_client.py` `decode_packet(data, bd_max=12000)` (S2C MaxPacket=1500).
Handle map: `tools/netfields_u_global.txt` (ROPlayerController ClassNetCache, maxHandle = 531).
First field of every bunch = `SerializeInt(handle, 531)`; `handle` indexes directly into the netfields map.

Join lands at frame ~1477; the PlayerController channel (ch2) **opens at frame 1484** (pid75, reliable open bunch).

## Decode is verified correct
`ClientShowTeamSelect` (handle 206) is sent in a 9-bit reliable bunch (`ce00`) and `SerializeInt(h,531)`
consumes exactly 9 bits to yield 206 — a clean fit, confirming bit alignment and the handle map.

---

## A. The reliable ch2 sequence = the login / team-select drive (chSeq order)

These are the bunches that actually drive the client. chSeq is strictly monotonic 1..47 with no gaps,
which by itself proves ch2 is the PlayerController channel and the framing decode is correct.

| sq | frame | t (s) | bits | handle | RPC / field | notes |
|----|-------|-------|------|--------|-------------|-------|
| 1  | 1484  | 103.78 | 925 | (open header) | **channel OPEN** | open bunch: actor header (class=ROPlayerController + Location + NetPlayerIndex) + initial property snapshot. First-handle decode of an OPEN bunch is meaningless (the "h=352" is header bits, not a field). |
| 2  | 1515  | 104.98 | 1401 | 56  | TeamMessage | first RPC bunch — large; the post-join system/team message batch |
| 3  | 1525  | 105.60 | 614 | 112 | ClientMutePlayer | |
| 4  | 1537  | 105.91 | 74  | 112 | ClientMutePlayer | |
| 5  | 1579  | 106.52 | 74  | 112 | ClientMutePlayer | |
| 6  | 1605  | 106.95 | 74  | 112 | ClientMutePlayer | |
| 7  | 1609  | 107.00 | 74  | 112 | ClientMutePlayer | |
| **8**  | **1637** | **107.36** | **9** | **206** | **ClientShowTeamSelect** | **<<< opens the team-select menu** |
| 9  | 1659  | 107.65 | 22  | 41  | ClientGotoState | puts PC FSM into the menu/spectate state |
| 10 | 1671  | 107.80 | 9   | 405 | ForceEscapeSceneUpdate | |
| 11 | 1702  | 108.22 | 74  | 112 | ClientMutePlayer | |
| 12 | 1712  | 108.35 | 74  | 112 | ClientMutePlayer | |
| 13 | 1815  | 109.64 | 22  | 41  | ClientGotoState | |
| 14 | 1851  | 110.12 | 74  | 113 | ClientUnmutePlayer | |
| 15 | 1876  | 110.42 | 74  | 113 | ClientUnmutePlayer | |
| 16 | 1973  | 111.63 | 74  | 112 | ClientMutePlayer | |
| 17 | 1977  | 111.68 | 22  | 41  | ClientGotoState | |
| 18 | 2138  | 113.76 | 22  | 41  | ClientGotoState | |
| 19 | 2154  | 113.96 | 74  | 112 | ClientMutePlayer | |
| 20 | 2296  | 115.79 | 22  | 41  | ClientGotoState | |
| 21 | 2324  | 116.14 | 276 | 179 | ClientUpdateSpectatorCameraSpeed | spectate setup |
| 22 | 2332  | 116.44 | 18  | 430 | ClientClearAllCharges | |
| 23 | 2346  | 117.00 | 74  | 113 | ClientUnmutePlayer | |
| 24 | 2358  | 117.45 | 74  | 113 | ClientUnmutePlayer | |
| 25 | 2405  | 119.37 | 74  | 113 | ClientUnmutePlayer | |
| 26 | 2426  | 120.22 | 74  | 112 | ClientMutePlayer | |
| 27 | 2439  | 120.73 | 74  | 113 | ClientUnmutePlayer | |
| 28 | 2440  | 120.78 | 74  | 113 | ClientUnmutePlayer | |
| 29 | 2461  | 121.47 | 659 | 36  | ClientUpdateBestNextHosts | peer/host list |
| 30 | 2537  | 124.51 | 66  | 210 | ChangedRole | |
| 31 | 2554  | 124.76 | 133 | 87  | ClientSetViewTarget | spectator camera target |
| 32 | 2554  | 124.76 | 229 | 87  | ClientSetViewTarget | |
| 33 | 2559  | 124.95 | 90  | 87  | ClientSetViewTarget | |
| 34 | 2697  | 126.64 | 74  | 113 | ClientUnmutePlayer | |
| 35 | 2709  | 126.79 | 22  | 41  | ClientGotoState | |
| 36 | 2714  | 126.85 | 74  | 112 | ClientMutePlayer | |
| 37-39 | 2773-2797 | ~127.6-127.9 | 74 | 113 | ClientUnmutePlayer | |
| 40 | 2810  | 128.03 | 325 | 56  | TeamMessage | |
| 41 | 2818  | 128.12 | 74  | 113 | ClientUnmutePlayer | |
| 42 | 2843  | 128.43 | 89  | 46  | ReceiveLocalizedMessage | |
| 43 | 2875  | 128.83 | 22  | 41  | ClientGotoState | |
| 44 | 2875  | 128.83 | 171 | 61  | ClientSetCameraMode | |
| 45-46 | 2924-2949 | ~129.4-129.7 | 74 | 113 | ClientUnmutePlayer | |
| 47 | 2982  | 130.17 | 9   | 263 | ClientAlertDeadSquadLeader | |

### The minimal causal chain to team-select
1. **OPEN ch2** (sq1) — server opens the PlayerController channel; open bunch carries the actor
   header (ROPlayerController class ref + Location + NetPlayerIndex) and the PC's initial property snapshot.
2. **TeamMessage** (sq2) — post-join message batch (not required for the menu).
3. **ClientShowTeamSelect** (sq8, handle **206**) — **this is the RPC that opens the team-select menu.**
4. **ClientGotoState** (sq9, handle 41) — moves the PC state machine into the menu/spectating state.
   Re-sent periodically (sq13/17/18/20/35/43) as the client cycles states.
5. Everything else (Mute/Unmute spam, ViewTarget, CameraMode, SpectatorCameraSpeed) is spectator/voice
   housekeeping that runs while the joined client watches the live match from the menu.

---

## B. Key finding: the hypothesized RPCs are NOT used to open team-select

Scanned all ch2 bunches (reliable + unreliable) frames 1480-3000:

- **ClientSetHUD (handle 45): NOT SENT** on ch2 in this phase.
- **ClientRestart (handle 85): NOT SENT** on ch2 in this phase.
- **ChangedTeams (handle 172): NOT SENT** on ch2 in this phase.

The menu is driven purely by **ClientShowTeamSelect (206) + ClientGotoState (41)**. This corrects the
working assumption (and the `ChangedTeams (team-select menu RPC)` note in commit eea217e): ChangedTeams
is a *post-team-pick* confirmation RPC, not the menu opener. ClientShowTeamSelect is the opener.

Handle histogram (reliable ch2, frames 1480-3000):
```
h= 36 x 1   ClientUpdateBestNextHosts      h=179 x 1   ClientUpdateSpectatorCameraSpeed
h= 41 x 7   ClientGotoState                h=206 x 1   ClientShowTeamSelect
h= 46 x 1   ReceiveLocalizedMessage        h=210 x 1   ChangedRole
h= 56 x 2   TeamMessage                    h=263 x 1   ClientAlertDeadSquadLeader
h= 61 x 1   ClientSetCameraMode            h=352 x 1   (open-header artifact, sq1)
h= 87 x 3   ClientSetViewTarget            h=405 x 1   ForceEscapeSceneUpdate
h=112 x 11  ClientMutePlayer               h=430 x 1   ClientClearAllCharges
h=113 x 14  ClientUnmutePlayer
```

---

## C. Interleaved UNRELIABLE ch2 traffic (background, not part of the login control flow)

Between the reliable bunches, ch2 also carries a heavy stream of **unreliable** bunches (chSeq=0).
First-handle decode of these (value streams not fully walked, so mapping is by first handle only):

| handle | field | meaning |
|--------|-------|---------|
| 51  | WwiseClientHearSound (func) | unreliable 3D/positional sound events streamed to the spectating client (most frequent) |
| 23  | PlayerReplicationInfo (Controller prop) | PC's own PRI object-ref refreshes (many 20-bit `176a00` bunches) |
| 320 | RepHitInfo (ROPlayerController prop) | hit-info struct updates from the live match |
| 317 | RepBattleChatter (ROPlayerController prop) | battle-chatter struct updates |
| 322 | DelayedKillMessages (ROPlayerController prop) | kill-message struct updates |
| 324 | WeaponProficiencyCurrent (ROPlayerController prop) | proficiency struct (one large 1372-bit dump at f1549) |

These are normal "watching a live game from the menu" replication and are **not** required to render the
team-select menu. They confirm ch2 stays hot with unreliable property/sound traffic throughout.

---

## D. Implications for the emulator

- To reproduce the real flow, the server must, on ch2 (the PlayerController channel) after Join:
  1. Open the channel with a proper actor open header (class + Location + NetPlayerIndex) + initial PC
     property snapshot (sq1).
  2. Send a reliable **ClientShowTeamSelect (handle 206)** bunch — 9-bit payload `ce00` (handle only,
     no args).
  3. Send a reliable **ClientGotoState (handle 41)** bunch (22 bits) to enter the menu state.
- Do **not** rely on ChangedTeams / ClientSetHUD / ClientRestart to open the menu — the real server
  does not send them here.
- The **black-square team buttons** are a separate issue: the button artwork is driven by
  GRI / ROTeamInfo property *values* replicated on channels ch54 (GRI) and ch21/56/76 (TeamInfo),
  **not** by ch2. The ch2 sequence above only opens the menu shell; the button content comes from the
  GRI/TeamInfo channels. Replicate those property values to fix the squares.

Raw first-handle dump of the first 220 ch2 bunches: `docs/re/_ch2_raw.txt`.
