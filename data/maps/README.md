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
  Defines spawn points in world coordinates:

  ```
  # x y z (one per line)
  1234.5 67.2 -89.0
  1278.0 65.1 -89.0
  ```

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