# RS2V Server — Maps Directory README

This document describes the **`maps/`** directory of the RS2V Custom Server: its purpose, structure, file formats, usage instructions, and best practices for adding or modifying maps.

## Overview

The `maps/` directory contains map definition files and assets for all game maps supported by the server. It serves two primary roles:

1. **Metadata Storage**: Holds `maps.ini` which defines map-level metadata (display names, supported modes, player counts, environmental settings, vote weights, etc.).
2. **Map Assets**: Contains the actual `.umap` files (Unreal Engine map packages) and any auxiliary spawn-point definitions, lighting presets, or configuration overrides.

## Directory Structure

```
maps/
├── maps.ini
├── /
│   ├── .umap
│   ├── spawns.txt
│   ├── lighting.json
│   └── README.md
├── /
│   ├── .umap
│   ├── spawns.txt
│   └── README.md
├── global_spawns.txt
└── README.md
```

- **`maps.ini`**: Central INI file listing every map with metadata sections.  
- **`/`**: Subdirectory per map for assets and overrides.  
  - **`.umap`**: Unreal Engine map package.  
  - **`spawns.txt`**: Spawn-point definitions (one coordinate per line).  
  - **`lighting.json`**: Optional lighting/time-of-day overrides.  
  - **`README.md`**: Map-specific notes, history, and optimization tips.  
- **`global_spawns.txt`**: Default spawn-point templates used when individual `spawns.txt` is missing.  
- **`README.md`**: This file.

## `maps.ini` Specification

Each map is defined under a `[MapID]` section. Keys:

| Key               | Type     | Description                                                                 |
|-------------------|----------|-----------------------------------------------------------------------------|
| `display_name`    | string   | Human-readable map title.                                                    |
| `file`            | string   | Relative path to `.umap` (e.g., `MapID/MapID.umap`).                         |
| `supported_modes` | list     | Comma-separated ModeIDs matching `game_modes.ini`.                          |
| `min_players`     | int      | Minimum players required to vote/load the map.                              |
| `max_players`     | int      | Maximum supported players.                                                   |
| `default_mode`    | string   | Fallback ModeID if no override specified.                                   |
| `time_of_day`     | string   | `day`, `dusk`, `night`, or `dawn`.                                           |
| `weather`         | string   | `clear`, `rain`, or `fog`.                                                   |
| `vote_weight`     | int      | Likelihood weight 1–100 for map voting.                                      |
| `description`     | string   | Optional brief map synopsis.                                                 |

### Example

```ini
[carcassonne]
display_name    = Carcassonne
file            = carcassonne/carcassonne.umap
supported_modes = Conquest,HotZone,Domination
min_players     = 2
max_players     = 64
default_mode    = Conquest
time_of_day     = day
weather         = clear
vote_weight     = 80
description     = Urban combat with narrow alleys and elevated walls.
```

## Map Subdirectories

Each map directory (`maps//`) may include:

- **`.umap`**  
  Core UE3 map file. Must match the `file` entry in `maps.ini`.

- **`spawns.txt`**  
  Defines spawn points in world coordinates. The optional Territory objective
  phase bounds are inclusive; use `-1` (or omit the field) for an unbounded
  side. Non-Territory modes ignore phase bounds:

  ```text
  # x y z [teamId [minTerritoryPhase [maxTerritoryPhase [retailSpawnVolumeRef]]]]
  1234.5 67.2 -89.0 1          # legacy/unbounded
  1278.0 65.1 -89.0 1 0 0     # phase 0 only
  2200.0 80.0 -75.0 1 1 3     # phases 1 through 3
  3400.0 95.0 -60.0 2 2 -1 301195 # phase 2 onward + cooked spawn volume
  ```

  `retailSpawnVolumeRef` is the optional **canonical** static PackageMap object
  index for the cooked `ROVolumePlayerStartGroup` represented by the row. It is
  map-build specific and must be capture/package grounded; use `0` or omit it
  when the reference is unknown. At send time, the emulator rebases nonzero
  refs through the connection's frozen replication-artifact layout (for
  example, the installed layout currently contributes `+5`). The authored
  fixture remains canonical, and the emulator never fabricates an index.

- **`objectives.txt`**
  Defines server capture zones and, optionally, the retail client's cooked
  `ROObjective` identity:

  ```text
  # name x y z radius phase tunnel [tunnelX tunnelY tunnelZ]
  #      [clientSlot cookedRepIndex] [enabled] [connectedToBase]
  #      [initialOwner] [captureSeconds] [pointValue] [homeTeam]
  #      [adjacentClientSlotsCsv]
  Beach -11950.55 5238.709 -544.122 35 0 0 0 0 1 0
  "Governor's House" 1810.98 -2897.73 191.09 30 0 0 2 3 1 0 0 10
  ```

  `clientSlot` is `ROObjective.ObjIndex` (0-15) and `cookedRepIndex` is
  `ROObjective.ObjRepIndex` (0-254). They must be supplied as a pair and be
  unique within the map. Omit both for server-only fallback zones. For a
  non-tunnel objective that supplies retail metadata, keep the explicit `0`
  tunnel field before the pair. Quote names containing spaces. `enabled`,
  `connectedToBase`, and `initialOwner` default to `1`, `0`, and neutral (`0`)
  respectively. `initialOwner` accepts `0` (neutral), `1` (South/US), or `2`
  (North/NVA). A positive `captureSeconds` sets the one-player capture time;
  omitting it keeps the default 10 seconds. Supremacy maps may then specify an
  objective `pointValue`, a `homeTeam` (`0`, `1`, or `2`), and comma-separated
  adjacent client slots (for example `0,1,3,4`; use `-` for none).

- **`objective_lockdown.txt`** (optional)
  Supplies package-grounded cooked `ROObjective` lockdown properties without
  extending the positional objective format:

  ```text
  # clientSlot enabled LockDownTime16 LockDownTime32 LockDownTime64
  0 1 500 500 500
  4 0 500 500 500
  ```

  `clientSlot` must identify an objective mapped by that map's
  `objectives.txt`; `enabled` is `0` or `1`; each time is an integer number of
  seconds in the inclusive range 0-86400. The file is parsed transactionally:
  an invalid, duplicate, trailing, or unmapped row rejects the whole file and
  leaves all lockdown metadata unknown. There is intentionally no global
  fallback because these values belong to a specific cooked map package.

- **`bot_navigation.txt`** (optional)
  Supplies one exact-map/exact-mode deterministic waypoint graph for headless
  bots. There is intentionally no global fallback. The format is strict and
  versioned:

  ```text
  version 1
  mode Supremacy
  # maxEdgeLength maxEndpointSnapDistance maxDirectRouteLength allowDirectFallback
  config 6000 3500 0 0
  node 1 1969.750 645.147 30.355
  node 2 -1891.356 -639.598 58.494
  edge 1 2 1
  ```

  `edge` rows are directed unless their final value is `1`, which installs both
  directions. Node ids must be unique nonzero 32-bit values. Every edge must
  reference two different declared nodes and fit `maxEdgeLength`.
  `maxEndpointSnapDistance` independently bounds the un-authored chord from a
  bot or objective to its nearest node. `allowDirectFallback` is `0` or `1`; a
  zero `maxDirectRouteLength` intentionally disables direct routing. Unknown
  directives, duplicate singleton rows, duplicate directed arcs, trailing
  tokens, non-finite values, unsupported modes, oversized graphs, or any invalid row reject the
  complete sidecar. Missing/rejected metadata preserves the server's legacy
  bounded direct-routing behavior; a graph is activated only when its declared
  mode matches the effective game mode.

- **`lighting.json`** (optional)  
  Overrides global lighting/time-of-day settings:

  ```json
  {
    "time_of_day": "dusk",
    "sun_intensity": 0.8,
    "ambient_color": [100, 100, 120]
  }
  ```

- **`README.md`**  
  Map-specific guidance: performance tips, known issues, community credits.

## Best Practices

1. **Consistent MapIDs**  
   - Use lowercase alphanumeric identifiers without spaces (e.g., `hill_400`).  
   - Directory name and INI section must match exactly.

2. **Spawn-Point Accuracy**  
   - Validate `spawns.txt` coordinates in-engine.  
   - Provide at least two spawns per team side.

3. **Metadata Clarity**  
   - Keep `description` concise (<120 characters).  
   - Update `vote_weight` based on playtesting.

4. **Lighting Overrides**  
   - Use `lighting.json` only for special cases (e.g., night maps).  
   - Otherwise rely on global `maps.ini` `time_of_day` key.

5. **Assets Versioning**  
   - Commit `.umap` files sparingly—only after major edits.  
   - Use separate branches for experimental map changes.

## Adding a New Map

1. **Create Directory**  
   ```bash
   mkdir maps/new_map
   cp path/to/new_map.umap maps/new_map/new_map.umap
   ```

2. **Define in `maps.ini`**  
   Add:

   ```ini
   [new_map]
   display_name    = New Battlefield
   file            = new_map/new_map.umap
   supported_modes = Conquest,Elimination
   min_players     = 2
   max_players     = 64
   default_mode    = Conquest
   time_of_day     = day
   weather         = clear
   vote_weight     = 50
   description     = A brand-new combat zone with dynamic cover.
   ```

3. **Add `spawns.txt`**  
   Generate spawn points and commit.

4. **Test in-game**  
   - Launch server locally.  
   - Cycle to `new_map`.  
   - Verify balance, performance, and visuals.

5. **Finalize**  
   - Update map’s `README.md` with notes.  
   - Push changes and notify team for playtesting.

## Troubleshooting

- **Map Fails to Load**  
  - Verify `file` path in `maps.ini`.  
  - Ensure `.umap` exists and matches engine version.

- **Incorrect Spawn Locations**  
  - Check `spawns.txt` formatting (no extra whitespace).  
  - Confirm coordinates via in-game console `/getlocation`.

- **Vote Weight Ignored**  
  - Ensure `vote_weight` is integer 1–100.  
  - Restart server or reload config.

## References

- See **`docs/Development_Guide.md`** for build and testing workflows.  
- Consult **`ConfigManager`** and **`GameConfig`** code to understand how map data is consumed.

*End of `maps/README.md`*
