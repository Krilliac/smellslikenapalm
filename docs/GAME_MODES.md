# GAME_MODES.md — Game Modes Guide

This document provides a **comprehensive guide** to all game modes available in the RS2V Custom Server, including objectives, scoring mechanics, team configurations, timing, map compatibility, and instructions for creating custom game modes.

For configuration file details, see [CONFIGURATION.md](CONFIGURATION.md). For map information, see [MAPS.md](MAPS.md).

---

## Table of Contents

- [1. Overview](#1--overview)
- [2. Conquest](#2--conquest)
- [3. Elimination](#3--elimination)
- [4. Capture the Flag](#4--capture-the-flag)
- [5. Hot Zone](#5--hot-zone)
- [6. Domination](#6--domination)
- [7. Game Mode Comparison](#7--game-mode-comparison)
- [8. Creating Custom Game Modes](#8--creating-custom-game-modes)
- [9. Game Mode Internals](#9--game-mode-internals)

---

## 1 · Overview

The RS2V Custom Server ships with **five built-in game modes**, each offering distinct objectives, pacing, and strategic depth. Game modes are defined in `config/game_modes.ini` and can be customized or extended by server operators.

### How Game Modes Work

1. **Selection**: The server selects a game mode based on the current map's `default_mode` setting or the mode specified by an admin via `changemap`.
2. **Initialization**: The `RoundManager` configures the round using the mode's parameters (teams, time limit, score limit, respawn delay, win condition).
3. **Execution**: The game loop processes the mode's win condition logic each tick, checking for round/match completion.
4. **Transition**: When a round ends, the mode determines the winner, updates scores, and either starts the next round or ends the match.

### Win Condition Types

| Win Condition | Mechanic | Used By |
|---|---|---|
| `territory_control` | Hold strategic capture points to accumulate score. Points are earned per second for each held point. | Conquest |
| `elimination` | Last team with living players wins the round. No respawns. | Elimination |
| `capture_the_flag` | Steal the enemy flag and return it to your base. First to reach capture limit wins. | Capture the Flag |
| `zone_control` | Secure and hold a designated hot zone to accumulate points. Only one zone is active at a time. | Hot Zone |
| `multi_zone` | Control multiple capture points simultaneously. Points accumulate for each held zone. Supports 3+ teams. | Domination |

---

## 2 · Conquest

### Overview

Conquest is a large-scale territory control mode where two teams fight to capture and hold strategic points across the map. Points are earned passively for each capture point your team controls. The first team to reach the score limit wins.

### Configuration

```ini
[Conquest]
name             = Conquest
description      = Capture and hold strategic points until team score limit reached.
teams            = 2
players_per_team = 32
rounds_per_match = 1
win_condition    = territory_control
timelimit_sec    = 900
score_limit      = 1000
respawn_delay    = 5
map_list         = carcassonne, hill_400, rubber_plant
friendly_fire    = false
```

### Specifications

| Parameter | Value | Notes |
|---|---|---|
| **Teams** | 2 | Red (US Army) vs. Blue (NVA) |
| **Players per team** | 32 | Up to 64 total |
| **Rounds per match** | 1 | Single continuous round |
| **Win condition** | Territory Control | Score accumulates while holding points |
| **Time limit** | 15 minutes (900s) | Team with higher score wins if time expires |
| **Score limit** | 1000 | First team to reach this wins immediately |
| **Respawn delay** | 5 seconds | Players respawn after a short delay |
| **Friendly fire** | Off | Teammates cannot damage each other |

### Mechanics

- **Capture Points**: Multiple strategic positions are spread across the map. Each point starts neutral or assigned to a team based on map design.
- **Capturing**: Stand near an uncaptured or enemy-held point without enemies present to begin capturing. Progress is shown as a capture bar. Multiple teammates near the point capture faster.
- **Score Accumulation**: Each held capture point generates score passively (typically 1 point per second per held point). More points held = faster score generation.
- **Contested Points**: A capture point cannot be captured while both teams have players in the capture zone. The point is "contested" and no progress is made.
- **Overtime**: If scores are tied when the time limit expires, overtime begins. The first team to break the tie wins.

### Compatible Maps

| Map | Max Players | Time of Day | Weather |
|---|---|---|---|
| Carcassonne | 64 | Day | Clear |
| Hill 400 | 48 | Dawn | Fog |
| Rubber Plant | 56 | Day | Rain |
| Village | 64 | Day | Rain |
| Coastal Assault | 64 | Dawn | Clear |

### Strategy Tips

- Control the majority of capture points rather than all of them to maintain a steady score advantage.
- Defend held points with at least one player to prevent back-captures.
- Communicate with your team to coordinate pushes on contested points.
- The center point is typically the most contested; flanking to capture side points can be more efficient.

---

## 3 · Elimination

### Overview

Elimination is a round-based team deathmatch mode with no respawns. The last team with surviving players wins the round. The match is a best-of-N series of rounds.

### Configuration

```ini
[Elimination]
name             = Elimination
description      = Team deathmatch: last team standing wins round; best of N rounds.
teams            = 2
players_per_team = 16
rounds_per_match = 9
win_condition    = elimination
timelimit_sec    = 600
respawn_delay    = 0
friendly_fire    = false
```

### Specifications

| Parameter | Value | Notes |
|---|---|---|
| **Teams** | 2 | Red vs. Blue |
| **Players per team** | 16 | Up to 32 total |
| **Rounds per match** | 9 | Best of 9 (first to 5 round wins) |
| **Win condition** | Elimination | Last team standing |
| **Time limit** | 10 minutes (600s) per round | Team with more survivors wins if time expires |
| **Score limit** | — | Not applicable |
| **Respawn delay** | 0 seconds | No respawns — dead players spectate |
| **Friendly fire** | Off | Teammates cannot damage each other |

### Mechanics

- **No Respawns**: Once a player dies, they are eliminated for the remainder of the round. They can spectate remaining teammates.
- **Round Win**: A round ends when all players on one team are eliminated. The surviving team earns one round win.
- **Match Win**: The first team to win a majority of rounds wins the match. In a best-of-9, the first team to win 5 rounds wins.
- **Time Limit**: If the per-round time limit expires, the team with more surviving players wins the round. If both teams have equal survivors, the round is a draw.
- **Economy**: There is no economy system. All players start each round with full equipment.

### Compatible Maps

| Map | Max Players | Time of Day | Weather |
|---|---|---|---|
| Hill 400 | 48 | Dawn | Fog |
| Hacienda | 64 | Dusk | Clear |
| Hill 937 | 64 | Night | Clear |
| Skirmish Field | 32 | — | — |

### Strategy Tips

- Play conservatively — your life matters more than getting kills.
- Use terrain and cover to your advantage, especially in fog and night conditions.
- Coordinate team movements to avoid being picked off individually.
- In later rounds, adapt your strategy based on what worked (or didn't) in earlier rounds.

---

## 4 · Capture the Flag

### Overview

Capture the Flag (CTF) is a classic objective mode where teams must steal the enemy flag from their base and return it to their own base. Each successful flag capture earns a point. The first team to reach the capture limit wins the round.

### Configuration

```ini
[CaptureTheFlag]
name             = Capture the Flag
description      = Steal the enemy flag and return it to your base.
teams            = 2
players_per_team = 24
rounds_per_match = 5
win_condition    = capture_the_flag
timelimit_sec    = 1200
score_limit      = 3
respawn_delay    = 10
friendly_fire    = true
```

### Specifications

| Parameter | Value | Notes |
|---|---|---|
| **Teams** | 2 | Red vs. Blue |
| **Players per team** | 24 | Up to 48 total |
| **Rounds per match** | 5 | Best of 5 (first to 3 round wins) |
| **Win condition** | Capture the Flag | First to reach capture limit |
| **Time limit** | 20 minutes (1200s) per round | Team with more captures wins if time expires |
| **Score limit** | 3 captures | First to 3 flag captures wins the round |
| **Respawn delay** | 10 seconds | Longer delay to increase tension |
| **Friendly fire** | On | Watch your fire around the flag carrier |

### Mechanics

- **Flag Locations**: Each team has a flag at their base. The flag is visible on the HUD.
- **Grabbing the Flag**: Walk over the enemy flag to pick it up. The flag carrier is marked on the map for both teams.
- **Capturing**: Carry the enemy flag back to your own flag stand while your flag is at your base. If your flag has been stolen, you cannot capture until it is recovered.
- **Dropping the Flag**: The flag carrier drops the flag when killed. Teammates of the flag carrier's team cannot pick up a dropped friendly flag — it remains on the ground.
- **Recovering the Flag**: Walk over your own dropped flag to instantly return it to your base.
- **Flag Reset Timer**: If a dropped flag is not picked up by the enemy team within 30 seconds, it is automatically returned to its base.
- **Friendly Fire**: Friendly fire is enabled by default in CTF to add tactical complexity around the flag carrier.

### Compatible Maps

| Map | Max Players | Time of Day | Weather |
|---|---|---|---|
| Rubber Plant | 56 | Day | Rain |
| Hill 937 | 64 | Night | Clear |
| Village | 64 | Day | Rain |

### Strategy Tips

- Always have players defending your flag. A successful capture requires your flag to be at your base.
- The flag carrier should be escorted by teammates — they are a high-priority target.
- Use the longer respawn delay strategically: eliminating defenders before a flag push gives your team a window.
- With friendly fire on, be careful when shooting near your own flag carrier.

---

## 5 · Hot Zone

### Overview

Hot Zone is a dynamic objective mode where teams fight to secure and hold a single designated zone on the map. Points accumulate while your team controls the zone. The zone's location may shift during the round to keep gameplay dynamic.

### Configuration

```ini
[HotZone]
name             = Hot Zone
description      = Secure and hold an objective zone to accumulate points.
teams            = 2
players_per_team = 32
rounds_per_match = 3
win_condition    = zone_control
timelimit_sec    = 600
score_limit      = 500
respawn_delay    = 7
friendly_fire    = false
map_list         = hacienda, hill_937, village
```

### Specifications

| Parameter | Value | Notes |
|---|---|---|
| **Teams** | 2 | Red vs. Blue |
| **Players per team** | 32 | Up to 64 total |
| **Rounds per match** | 3 | Best of 3 (first to 2 round wins) |
| **Win condition** | Zone Control | Hold the zone to earn score |
| **Time limit** | 10 minutes (600s) per round | Team with higher score wins if time expires |
| **Score limit** | 500 | First to 500 points wins the round |
| **Respawn delay** | 7 seconds | Medium delay balancing action and strategy |
| **Friendly fire** | Off | Teammates cannot damage each other |

### Mechanics

- **The Zone**: A single circular zone is marked on the map. It is visible to all players on the HUD and minimap.
- **Securing**: When only one team has players inside the zone, they "own" it. Score accumulates for the owning team.
- **Contesting**: If both teams have players inside the zone, it is contested and no score is earned by either team.
- **Zone Shifting**: After a configurable interval or when a score threshold is reached, the zone may move to a new location on the map. All players are notified.
- **Score Rate**: The more players your team has inside the zone (relative to the enemy), the faster points accumulate.

### Compatible Maps

| Map | Max Players | Time of Day | Weather |
|---|---|---|---|
| Carcassonne | 64 | Day | Clear |
| Rubber Plant | 56 | Day | Rain |
| Hacienda | 64 | Dusk | Clear |
| Hill 937 | 64 | Night | Clear |
| Coastal Assault | 64 | Dawn | Clear |

### Strategy Tips

- Control the zone with numbers. More players inside = faster scoring.
- Rotate players in and out of the zone to maintain presence while resupplying.
- When the zone shifts, move quickly to the new location. Early control gives a significant advantage.
- Use the 7-second respawn delay window to push into the zone after eliminating defenders.

---

## 6 · Domination

### Overview

Domination is a multi-zone control mode supporting **three or more teams**. Multiple capture points are active simultaneously, and teams earn points for each zone they control. The first team to reach the score limit wins.

### Configuration

```ini
[Domination]
name             = Domination
description      = Control multiple points; first to reach score limit wins.
teams            = 3
players_per_team = 20
rounds_per_match = 1
win_condition    = multi_zone
timelimit_sec    = 1200
score_limit      = 800
respawn_delay    = 5
friendly_fire    = false
```

### Specifications

| Parameter | Value | Notes |
|---|---|---|
| **Teams** | 3 | Three-way battle (unique to this mode) |
| **Players per team** | 20 | Up to 60 total |
| **Rounds per match** | 1 | Single continuous round |
| **Win condition** | Multi-Zone Control | Earn points by holding multiple zones |
| **Time limit** | 20 minutes (1200s) | Team with highest score wins if time expires |
| **Score limit** | 800 | First team to reach this wins immediately |
| **Respawn delay** | 5 seconds | Quick respawns to maintain action |
| **Friendly fire** | Off | Teammates cannot damage each other |

### Mechanics

- **Multiple Zones**: 3–5 capture zones are spread across the map. All zones are active simultaneously.
- **Three Teams**: Domination is the only built-in mode supporting three teams, creating a dynamic three-way conflict.
- **Capture Mechanics**: Zones are captured by having more team members inside than any other team. Capture speed scales with player advantage.
- **Score Generation**: Each held zone generates score independently. Holding more zones = faster total scoring.
- **Dominant Control**: A team controlling all zones triggers "Dominant Control" — a bonus scoring multiplier that rapidly increases their score.
- **Shifting Alliances**: The three-way dynamic means two teams may implicitly cooperate against the leading team, creating emergent gameplay.

### Compatible Maps

| Map | Max Players | Time of Day | Weather |
|---|---|---|---|
| Carcassonne | 64 | Day | Clear |
| Hill 400 | 48 | Dawn | Fog |
| Hacienda | 64 | Dusk | Clear |
| Village | 64 | Day | Rain |
| Skirmish Field | 32 | — | — |

### Strategy Tips

- Focus on controlling a majority of zones (3 out of 5) rather than trying to hold all of them.
- Watch both enemy teams. If one team is dominating, consider temporarily focusing fire on them.
- Spread your team across zones rather than grouping on one. Each zone generates independent score.
- The three-way dynamic means the leader changes frequently. Stay adaptable.

---

## 7 · Game Mode Comparison

### At a Glance

| Feature | Conquest | Elimination | CTF | Hot Zone | Domination |
|---|---|---|---|---|---|
| **Teams** | 2 | 2 | 2 | 2 | 3 |
| **Max Players** | 64 | 32 | 48 | 64 | 60 |
| **Respawns** | Yes (5s) | No | Yes (10s) | Yes (7s) | Yes (5s) |
| **Rounds** | 1 | 9 | 5 | 3 | 1 |
| **Objective** | Hold points | Survive | Capture flag | Hold zone | Hold zones |
| **Pace** | Medium | Slow/tactical | Medium | Fast | Medium |
| **Friendly Fire** | No | No | Yes | No | No |
| **Maps** | 5 | 4 | 3 | 5 | 5 |

### Recommended For

| Scenario | Recommended Mode |
|---|---|
| Large servers (48–64 players) | Conquest, Hot Zone |
| Competitive play | Elimination |
| Small servers (16–32 players) | Elimination, Skirmish Field maps |
| Objective-focused gameplay | Capture the Flag, Hot Zone |
| Three-way battles | Domination |
| New players | Conquest (forgiving respawns, clear objectives) |

---

## 8 · Creating Custom Game Modes

### Step 1: Define the Mode in game_modes.ini

Add a new section to `config/game_modes.ini`:

```ini
[LastStand]
name             = Last Stand
description      = Defend the base against waves of attackers.
teams            = 2
players_per_team = 16
rounds_per_match = 3
win_condition    = elimination
timelimit_sec    = 480
respawn_delay    = 15
friendly_fire    = true
map_list         = hill_400, hacienda
```

### Step 2: Choose a Win Condition

Your custom mode must use one of the existing win condition types:

| Win Condition | Best For |
|---|---|
| `territory_control` | Modes involving capturing and holding points with score generation |
| `elimination` | Modes where the goal is to eliminate all enemy players (no respawns) |
| `capture_the_flag` | Modes involving object retrieval and delivery |
| `zone_control` | Modes focused on a single contested area |
| `multi_zone` | Modes with multiple simultaneous objectives (supports 3+ teams) |

### Step 3: Update Map Compatibility

Update `config/maps.ini` to include your new mode in the `supported_modes` of applicable maps:

```ini
[hill_400]
supported_modes = Conquest,Elimination,Domination,LastStand
```

### Step 4: Verify

Restart the server (or wait for hot-reload if supported) and use the `changemap` command with a compatible map to test your new mode.

### Advanced: Custom Win Conditions via Scripting

For win conditions not covered by the built-in types, you can implement custom logic using the C# scripting system. See [SCRIPTING.md](SCRIPTING.md) for details on:

- Subscribing to game events (`OnTick`, `OnPlayerKilled`, `OnObjectiveCaptured`)
- Modifying round state programmatically
- Creating custom HUD indicators for your mode's objectives

---

## 9 · Game Mode Internals

This section covers the internal architecture for developers. For server operators, the sections above provide all necessary information.

### Architecture

Game modes interact with the following subsystems:

```
GameServer
├── RoundManager
│   ├── GameMode (abstract base)
│   │   ├── ConquestMode     (territory_control)
│   │   ├── EliminationMode  (elimination)
│   │   ├── CTFMode          (capture_the_flag)
│   │   ├── HotZoneMode      (zone_control)
│   │   └── DominationMode   (multi_zone)
│   ├── ObjectiveTracker
│   └── ScoreManager
├── TeamManager
│   ├── Team[0..N]
│   └── AutoBalancer
├── PlayerManager
│   ├── SpawnManager
│   └── RespawnQueue
└── MapManager
    └── ObjectiveRegistry
```

### Game Mode Lifecycle

```
1. Mode Selection (from maps.ini default_mode or admin command)
      │
2. Mode Initialization (RoundManager::SetGameMode)
      │  ├── Configure teams, time limits, score limits
      │  └── Register win condition checker
      │
3. Round Start (RoundManager::StartRound)
      │  ├── Reset scores
      │  ├── Spawn players
      │  └── Activate objectives
      │
4. Game Loop (every tick)
      │  ├── Process player inputs
      │  ├── Update objectives (capture progress, zone control)
      │  ├── Check win condition
      │  └── Broadcast state updates
      │
5. Round End (triggered by win condition or time limit)
      │  ├── Determine winner
      │  ├── Update match score
      │  └── Either start next round or end match
      │
6. Match End (all rounds complete or early win)
      └── Report results, transition to next map
```

### Key Source Files

| File | Responsibility |
|---|---|
| `src/Game/GameMode.cpp` | Abstract base class for all game modes |
| `src/Game/SkirmishMode.cpp` | Elimination-style mode implementation |
| `src/Game/SupremacyMode.cpp` | Territory/zone control implementations |
| `src/Game/TerritoryMode.cpp` | Multi-zone territory implementation |
| `src/Game/MapManager.cpp` | Map loading and objective registry |

---

**End of GAME_MODES.md**

For map details and compatibility, see [MAPS.md](MAPS.md). For configuration, see [CONFIGURATION.md](CONFIGURATION.md).
