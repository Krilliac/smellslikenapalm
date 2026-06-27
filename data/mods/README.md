# RS2V Server — Mods Directory

This directory is scanned at startup by `ModManager` for loose `.pak` gameplay
mods. Any `*.pak` file placed here (recursively) is registered as a gameplay mod
and included in the client content manifest.

Mods and cosmetic assets distributed via Steam Workshop are declared in
`config/workshop_items.txt` instead and are resolved relative to the data
directory (see `[Workshop]` in `config/server.ini`).

See `docs/MAPS.md` §6 for the full Workshop / mod / asset workflow.
