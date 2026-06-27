# MAPS.md — Maps & Map Management Guide

This document provides a **comprehensive reference** for all built-in maps in the RS2V Custom Server, including map properties, game mode compatibility, rotation management, adding custom maps, Steam Workshop integration, and the map voting system.

For game mode details, see [GAME_MODES.md](GAME_MODES.md). For configuration file format, see [CONFIGURATION.md](CONFIGURATION.md).

---

## Table of Contents

- [1. Overview](#1--overview)
- [2. Built-in Maps](#2--built-in-maps)
  - [2.1 Carcassonne](#21-carcassonne)
  - [2.2 Hill 400](#22-hill-400)
  - [2.3 Rubber Plant](#23-rubber-plant)
  - [2.4 Hacienda](#24-hacienda)
  - [2.5 Hill 937](#25-hill-937)
  - [2.6 Village](#26-village)
  - [2.7 Skirmish Field](#27-skirmish-field)
  - [2.8 Coastal Assault](#28-coastal-assault)
- [3. Map Properties Reference](#3--map-properties-reference)
- [4. Map Rotation](#4--map-rotation)
- [5. Adding Custom Maps](#5--adding-custom-maps)
- [6. Steam Workshop Integration](#6--steam-workshop-integration)
- [7. Map Voting System](#7--map-voting-system)
- [8. Map Administration](#8--map-administration)

---

## 1 · Overview

The RS2V Custom Server ships with **eight built-in maps**, each designed for specific game modes, player counts, and atmospheric conditions. Maps are defined in `config/maps.ini` and store their asset files in the `data/maps/` directory (configurable via `[DataPaths].maps_path` in `server.ini`).

### Map File Format

Maps use the Unreal Engine `.umap` format. Each map consists of:
- **Geometry**: Terrain, structures, and static meshes
- **Spawn Points**: Per-team spawn locations referenced by `teams.ini`
- **Objectives**: Capture points, flag stands, and zone markers used by game modes
- **Lighting**: Baked and dynamic lighting matching the map's time-of-day setting
- **Audio**: Ambient sounds, weather effects, and environmental audio

### How Maps Are Loaded

1. The server reads `config/maps.ini` at startup and registers all map sections.
2. The `MapManager` validates that each map's `.umap` file exists in `maps_path`.
3. The initial map is the first entry in the rotation (or specified by command line).
4. Map transitions load the new `.umap` file, reset game state, and initialize the selected game mode's objectives.

---

## 2 · Built-in Maps

### 2.1 Carcassonne

| Property | Value |
|---|---|
| **MapID** | `carcassonne` |
| **Display Name** | Carcassonne |
| **File** | `carcassonne.umap` |
| **Supported Modes** | Conquest, HotZone, Domination |
| **Default Mode** | Conquest |
| **Players** | 2–64 |
| **Time of Day** | Day |
| **Weather** | Clear |
| **Vote Weight** | 80 (high) |

**Description:** A large urban map featuring multiple capture points spread across narrow streets, plazas, and multi-story buildings. The dense urban environment creates intense close-quarters combat around capture points with long sightlines between districts. Ideal for large-scale Conquest matches where teams must control and hold multiple zones simultaneously.

**Key Features:**
- Multiple capture points at strategic intersections
- Multi-story buildings providing elevated positions
- Narrow streets creating natural chokepoints
- Open plazas for vehicle movement
- Strong flanking routes through alleys and buildings

---

### 2.2 Hill 400

| Property | Value |
|---|---|
| **MapID** | `hill_400` |
| **Display Name** | Hill 400 |
| **File** | `hill_400.umap` |
| **Supported Modes** | Conquest, Elimination, Domination |
| **Default Mode** | Elimination |
| **Players** | 2–48 |
| **Time of Day** | Dawn |
| **Weather** | Fog |
| **Vote Weight** | 60 (medium) |

**Description:** A hillside map set at dawn with heavy fog, reducing visibility and emphasizing close-range combat and sound-based awareness. The elevation changes create natural defensive positions and challenging attack routes. The fog makes this map particularly well-suited for tactical Elimination rounds where stealth matters.

**Key Features:**
- Significant elevation changes with hilltop defensive positions
- Dense fog limiting visibility to medium-short range
- Dawn lighting creating atmospheric silhouettes
- Terrain-based cover with trenches and foxholes
- Multiple approach routes up the hillside

---

### 2.3 Rubber Plant

| Property | Value |
|---|---|
| **MapID** | `rubber_plant` |
| **Display Name** | Rubber Plant |
| **File** | `rubber_plant.umap` |
| **Supported Modes** | Conquest, HotZone, CaptureTheFlag |
| **Default Mode** | Conquest |
| **Players** | 2–56 |
| **Time of Day** | Day |
| **Weather** | Rain |
| **Vote Weight** | 70 (medium-high) |

**Description:** An industrial rubber plantation with a mix of open fields, processing buildings, and dense vegetation. The rain affects visibility and creates ambient noise that can mask footsteps. The map's symmetrical layout makes it well-suited for Capture the Flag, with bases at opposite ends and multiple approach routes through the plantation.

**Key Features:**
- Mix of open fields and dense plantation rows
- Industrial processing buildings as key control points
- Rain reducing visibility and masking audio cues
- Symmetrical layout ideal for CTF
- Vehicle-accessible main roads with infantry-only shortcuts

---

### 2.4 Hacienda

| Property | Value |
|---|---|
| **MapID** | `hacienda` |
| **Display Name** | Hacienda |
| **File** | `hacienda.umap` |
| **Supported Modes** | HotZone, Elimination, Domination |
| **Default Mode** | HotZone |
| **Players** | 2–64 |
| **Time of Day** | Dusk |
| **Weather** | Clear |
| **Vote Weight** | 75 (medium-high) |

**Description:** A sprawling estate compound set at dusk, featuring a central hacienda building surrounded by courtyards, gardens, outbuildings, and perimeter walls. The warm dusk lighting creates long shadows and atmospheric gameplay. The central compound is a natural Hot Zone objective.

**Key Features:**
- Central hacienda building as the primary objective
- Multiple courtyards and garden areas
- Dusk lighting with dramatic shadows
- Clear weather maintaining good visibility
- Perimeter walls creating distinct inside/outside zones

---

### 2.5 Hill 937

| Property | Value |
|---|---|
| **MapID** | `hill_937` |
| **Display Name** | Hill 937 |
| **File** | `hill_937.umap` |
| **Supported Modes** | HotZone, CaptureTheFlag, Elimination |
| **Default Mode** | HotZone |
| **Players** | 2–64 |
| **Time of Day** | Night |
| **Weather** | Clear |
| **Vote Weight** | 50 (standard) |

**Description:** A heavily contested hilltop position set at night with clear skies. The darkness dramatically changes combat dynamics — muzzle flashes reveal positions, and sound becomes critical for awareness. The hill's summit serves as a natural Hot Zone objective.

**Key Features:**
- Night setting with moonlight and artificial lighting
- Hilltop summit as the primary objective
- Muzzle flash visibility in darkness adds tactical depth
- Clear night sky with star navigation
- Dense jungle vegetation on the hillside

---

### 2.6 Village

| Property | Value |
|---|---|
| **MapID** | `village` |
| **Display Name** | Village |
| **File** | `village.umap` |
| **Supported Modes** | Conquest, Domination, CaptureTheFlag |
| **Default Mode** | Conquest |
| **Players** | 2–64 |
| **Time of Day** | Day |
| **Weather** | Rain |
| **Vote Weight** | 65 (medium) |

**Description:** A rural Vietnamese village with rice paddies, thatched-roof buildings, a central market area, and surrounding jungle. Rain creates muddy terrain and reduced visibility. The village layout provides diverse combat environments from open paddy fields to tight building interiors.

**Key Features:**
- Traditional Vietnamese village architecture
- Open rice paddies with minimal cover
- Dense village center with building-to-building combat
- Surrounding jungle providing flanking routes
- Rain creating atmospheric and gameplay effects

---

### 2.7 Skirmish Field

| Property | Value |
|---|---|
| **MapID** | `skirmish_field` |
| **Display Name** | Skirmish Field |
| **File** | `skirmish_field.umap` |
| **Supported Modes** | Elimination, Domination |
| **Default Mode** | Elimination |
| **Players** | 2–32 |
| **Time of Day** | _(not set)_ |
| **Weather** | _(not set)_ |
| **Vote Weight** | 50 (standard) |

**Description:** A compact map designed for smaller player counts. The tight layout forces frequent engagements and fast-paced gameplay. Ideal for competitive Elimination matches with smaller teams.

**Key Features:**
- Compact map size for intense action
- Designed for 16–32 players
- Fast-paced gameplay with short engagement distances
- Minimal downtime between encounters
- Balanced sightlines for both teams

---

### 2.8 Coastal Assault

| Property | Value |
|---|---|
| **MapID** | `coastal_assault` |
| **Display Name** | Coastal Assault |
| **File** | `coastal_assault.umap` |
| **Supported Modes** | Conquest, HotZone |
| **Default Mode** | Conquest |
| **Players** | 2–64 |
| **Time of Day** | Dawn |
| **Weather** | Clear |
| **Vote Weight** | 55 (medium) |

**Description:** A beachfront assault map with one team attacking from the sea and the other defending coastal fortifications. The asymmetric layout creates a distinct attacker/defender dynamic. Dawn lighting provides growing visibility as the round progresses.

**Key Features:**
- Asymmetric attacker/defender layout
- Beach approach with minimal cover for attackers
- Coastal fortifications and bunkers for defenders
- Dawn lighting increasing visibility over time
- Multiple attack paths from the beach inland

---

## 3 · Map Properties Reference

Every map section in `config/maps.ini` supports these properties:

| Property | Type | Required | Default | Description |
|---|---|---|---|---|
| `display_name` | string | Yes | — | Human-readable name shown in server browser and UI |
| `file` | path | Yes | — | Map asset filename relative to `maps_path` |
| `supported_modes` | string | Yes | — | Comma-separated list of compatible game mode IDs |
| `min_players` | integer | No | `2` | Minimum players required to start |
| `max_players` | integer | No | `64` | Maximum player capacity for this map |
| `default_mode` | string | No | _(first mode)_ | Default game mode when no mode is specified |
| `time_of_day` | enum | No | _(unset)_ | Lighting setting: `day`, `dusk`, `night`, `dawn` |
| `weather` | enum | No | _(unset)_ | Weather effect: `clear`, `rain`, `fog` |
| `vote_weight` | integer | No | `50` | Map vote probability weight (1–100) |
| `description` | string | No | _(empty)_ | Brief map description |

### Time of Day Effects

| Setting | Visibility | Lighting | Gameplay Impact |
|---|---|---|---|
| `day` | Full | Bright, even lighting | Standard combat visibility |
| `dusk` | Reduced | Warm tones, long shadows | Shadows create concealment |
| `night` | Limited | Moonlight, artificial lights | Muzzle flash reveals position |
| `dawn` | Increasing | Gradually brightening | Visibility changes during round |

### Weather Effects

| Setting | Visibility | Audio | Movement |
|---|---|---|---|
| `clear` | Full | Normal | Standard |
| `rain` | Reduced | Rain noise masks footsteps | Mud may slow movement |
| `fog` | Significantly reduced | Muffled sounds | Standard |

---

## 4 · Map Rotation

### Default Rotation

The map rotation follows the order of sections in `config/maps.ini`. After the last map, the rotation loops back to the first.

Default rotation order:
1. Carcassonne (Conquest)
2. Hill 400 (Elimination)
3. Rubber Plant (Conquest)
4. Hacienda (HotZone)
5. Hill 937 (HotZone)
6. Village (Conquest)
7. Skirmish Field (Elimination)
8. Coastal Assault (Conquest)

### Managing Rotation at Runtime

Use the `rotation` admin command to modify the rotation without editing files:

```
rotation list                      # View current rotation
rotation add desert_assault        # Add a map
rotation remove skirmish_field     # Remove a map
```

See [ADMIN_COMMANDS.md](ADMIN_COMMANDS.md) for full command details.

### Editing Rotation in Config

Reorder, add, or remove map sections in `config/maps.ini`. The file supports hot-reload — changes take effect at the next map transition without a server restart.

---

## 5 · Adding Custom Maps

### Step 1: Prepare the Map File

Place the `.umap` file in the `maps_path` directory (default: `data/maps/`):

```bash
cp my_custom_map.umap /opt/rs2v/data/maps/
```

### Step 2: Add to maps.ini

Add a new section to `config/maps.ini`:

```ini
[my_custom_map]
display_name    = My Custom Map
file            = my_custom_map.umap
supported_modes = Conquest,Elimination
min_players     = 2
max_players     = 48
default_mode    = Conquest
time_of_day     = day
weather         = clear
vote_weight     = 50
description     = A custom map for testing.
```

### Step 3: Verify

The map will be available at the next map rotation update (hot-reloaded). To test immediately:

```
changemap my_custom_map
```

### Guidelines for Custom Maps

- **Spawn points**: Ensure the map has valid spawn points for each team. Spawn point files are referenced in `config/teams.ini` via `spawn_points_file`.
- **Objective markers**: For modes like Conquest and HotZone, the map needs objective markers at valid positions.
- **Performance**: Test with the expected player count to ensure acceptable frame rates and network performance.
- **File size**: Keep `.umap` files as small as possible — clients need to download them on connect if using Workshop integration.

---

## 6 · Steam Workshop Integration

Custom maps, mods, and assets can be distributed via Steam Workshop. The server automatically downloads required Workshop items and ensures clients have them when connecting.

### Configuring Workshop Items

Add entries to `config/workshop_items.txt`:

```
# Custom Maps
123456789    map    desert_assault.umap    maps/desert_assault    # Desert Assault map

# Gameplay Mods
223344556    mod    night_ops.pak          mods/night_ops         # Night Operations

# Cosmetic Assets
556677889    asset  vietnam_uniforms.pak   assets/uniforms        # Vietnam-era uniforms
```

### Item Types

| Type | Description | Client Behavior |
|---|---|---|
| `map` | Custom map files (`.umap`) | Required download before playing on the map |
| `mod` | Gameplay modifications (`.pak`) | Required download before connecting |
| `asset` | Cosmetic content (`.pak`) | Required download before connecting |

### Workshop Item Format

```
<WorkshopID>    <Type>    <FileName>    <LocalPath>    # <Description>
```

| Field | Description |
|---|---|
| `WorkshopID` | Numeric Steam Workshop item ID (from the item's URL) |
| `Type` | `map`, `mod`, or `asset` |
| `FileName` | Expected filename after extraction |
| `LocalPath` | Server-side storage path relative to the data directory |
| `Description` | Optional human-readable note (after `#`) |

### Server Configuration

Workshop behavior is controlled by the `[Workshop]` section of `config/server.ini`:

```ini
[Workshop]
items_file       = config/workshop_items.txt
app_id           = 418460          ; Rising Storm 2: Vietnam
download_enabled = false           ; true = fetch missing items via steamcmd
steamcmd_path    = steamcmd
install_dir      =                 ; optional +force_install_dir
```

When `download_enabled = false` (the default), the server performs a **dry run**:
it logs the exact `steamcmd` command it *would* run for each missing item but
does not execute it. Set `download_enabled = true` (and provide a working
`steamcmd`) to have the server fetch missing items at startup. Admins can also
trigger this at runtime with `workshop download`.

### Notes

- The manifest is parsed at startup; `workshop reload` re-reads it without a restart.
- `workshop validate` re-checks which items are present on disk.
- Item presence is resolved against `<data_directory>/<local_path>/<file_name>`.
- Mods (`.pak`) are additionally discovered from `[DataPaths].mods_path` and
  registered by `ModManager`; see `mods` admin command.

---

## 7 · Map Voting System

### How Voting Works

At the end of each round (or match), the server presents players with a selection of maps to vote on. The most-voted map becomes the next map in the rotation.

### Vote Weight

The `vote_weight` property in `config/maps.ini` influences how likely a map is to appear as a vote option:

| Weight Range | Likelihood | Example |
|---|---|---|
| 80–100 | Very likely | Popular maps everyone enjoys |
| 60–79 | Likely | Well-liked maps |
| 40–59 | Standard | Average rotation maps |
| 20–39 | Uncommon | Niche or newer maps |
| 1–19 | Rare | Maps for variety only |

### Vote Configuration

The number of vote options and voting duration are controlled by the server's internal settings. Typically:
- **3–5 map options** are presented per vote
- **30 seconds** of voting time
- The current map is excluded from vote options
- Ties are broken by vote weight

---

## 8 · Map Administration

### Useful Admin Commands

| Command | Description | Example |
|---|---|---|
| `changemap <mapName>` | Switch to a map immediately | `changemap hill_937` |
| `rotation list` | View the current map rotation | `rotation list` |
| `rotation add <mapName>` | Defined in maps.ini; edit + `reload map` | `rotation add my_custom_map` |
| `rotation remove <mapName>` | Defined in maps.ini; edit + `reload map` | `rotation remove skirmish_field` |
| `startvote` | Start an end-of-round map vote now | `startvote` |
| `workshop [list\|reload\|validate\|download]` | Manage Steam Workshop items | `workshop list` |
| `mods` | List registered mods / assets | `mods` |
| `mutators` | List active and available mutators | `mutators` |
| `status` | See the current map and mode | `status` |

Players join an active vote with `/votemap` (show options) and `/votemap <number>` (cast).

See [ADMIN_COMMANDS.md](ADMIN_COMMANDS.md) for full command reference.

### Map Compatibility Matrix

| Map | Conquest | Elimination | CTF | HotZone | Domination |
|---|---|---|---|---|---|
| Carcassonne | Yes | — | — | Yes | Yes |
| Hill 400 | Yes | Yes | — | — | Yes |
| Rubber Plant | Yes | — | Yes | Yes | — |
| Hacienda | — | Yes | — | Yes | Yes |
| Hill 937 | — | Yes | Yes | Yes | — |
| Village | Yes | — | Yes | — | Yes |
| Skirmish Field | — | Yes | — | — | Yes |
| Coastal Assault | Yes | — | — | Yes | — |

---

**End of MAPS.md**

For game mode details, see [GAME_MODES.md](GAME_MODES.md). For configuration, see [CONFIGURATION.md](CONFIGURATION.md). For admin commands, see [ADMIN_COMMANDS.md](ADMIN_COMMANDS.md).
