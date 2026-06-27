# ADMIN_COMMANDS.md — Command System Reference

This document describes the RS2V Custom Server's **unified command system**: the
admin / dev / moderator / player / console / config / automation commands, the
permission model, and the three transports through which commands can be issued —
in-game chat, the local console, and a remote SOAP/HTTP endpoint built for
tooling and AI automation.

All commands are defined **once** in code (`src/Game/CommandManager` +
`src/Game/CommandHandlers.cpp`) and dispatched identically regardless of
transport. The permission gate is enforced centrally in `CommandManager`, so no
transport can bypass authorization.

For configuration file formats see [CONFIGURATION.md](CONFIGURATION.md); for the
security architecture see [SECURITY.md](SECURITY.md).

---

## Table of Contents

- [1. Architecture](#1--architecture)
- [2. Permission Levels](#2--permission-levels)
- [3. Transports](#3--transports)
  - [3.1 In-Game Chat](#31-in-game-chat)
  - [3.2 Local Console](#32-local-console)
  - [3.3 Remote SOAP/HTTP (automation & AI)](#33-remote-soaphttp-automation--ai)
- [4. Command Reference](#4--command-reference)
- [5. Admin List Management](#5--admin-list-management)
- [6. Automation Notes](#6--automation-notes)
- [7. Security Best Practices](#7--security-best-practices)

---

## 1 · Architecture

```
 in-game chat ─┐
 local console ─┼──> CommandContext ──> CommandManager ──> permission gate ──> handler
 remote SOAP ──┘      (who / level /        (one registry)     (central)        (one impl)
                       reply sink)
```

Each transport builds a **`CommandContext`** — who is asking, at what permission
level, and where output should go (`Reply`) — and calls `CommandManager::Execute`.
The manager resolves the command (name or alias, case-insensitive), checks the
caller's level against the command's minimum, writes an audit line, then invokes
the single handler implementation. Output is routed back through the context's
reply sink (chat message, stdout, or SOAP response body).

There is **one** command registry and **one** authorization gate. Adding a new
transport cannot accidentally grant access — it only supplies a context.

---

## 2 · Permission Levels

| Level | Name | Description |
|---|---|---|
| 0 | **Player** | Any connected player. Help + automation probes (`help`, `ping`, `echo`, `version`). |
| 1 | **Helper** | Read-only server info (`status`, `players`, `query`, `maps`, `mods`, `admins`). |
| 2 | **Moderator** | Player management (`kick`, `ban`*, `slay`, `god`, `tp`, `give`, `team`, `broadcast`, `startvote`). |
| 3 | **Admin** | Full server control (`changemap`, `reload`, `tickrate`, `config`, `eacmode`, `shutdown`, `workshop`). |
| 4 | **Dev** | Developer/diagnostic commands (`timescale`, `regen`, `spawnbot`). |
| 5 | **Console** | Local console and authenticated remote root — full trust. |

\* `ban`/`unban` are Admin-level; `kick`/`banlist` are Moderator-level.

Levels are **inclusive**: a higher level may run every command at or below it. A
player's level is resolved from `config/admin_list.txt` (see §5). The local
console always runs at **Console**; remote SOAP requests run at the level
configured by `RemoteAdmin.level` (default Admin).

---

## 3 · Transports

### 3.1 In-Game Chat

An authenticated player types a command prefixed with `/` in chat:

```
/status
/kick 76561198000000001 idle too long
/broadcast Round restarting in 30s
```

The player's permission level is resolved from their SteamID. Output is returned
privately to the issuing player. The chat-only macros `/me`, `/team`, and
`/votemap` remain handled by the chat layer; everything else dispatches through
the command system.

### 3.2 Local Console

Anyone with the server's standard input (a shell on the host) can type commands
directly. The console runs at **Console** level (full trust — controlling the
process already implies full control). Enabled by default; a headless launch with
no TTY simply sees EOF and disables console input without affecting the server.

```ini
[Console]
enabled = true
```

### 3.3 Remote SOAP/HTTP (automation & AI)

A small SOAP-over-HTTP endpoint lets off-box tooling — including AI automation —
drive the server remotely. It is **disabled** unless *both* a port and a password
are configured:

```ini
[RemoteAdmin]
soap_port = 27015        ; 0 = disabled
password  = a-strong-shared-secret
level     = 3            ; permission level for authenticated requests (0-5)
```

A request is an HTTP `POST` whose body carries a `<password>` and a `<command>`.
A minimal SOAP envelope works; the parser is namespace-agnostic and extracts the
two tags defensively:

```xml
<?xml version="1.0"?>
<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">
  <soap:Body>
    <ExecuteCommand>
      <password>a-strong-shared-secret</password>
      <command>status</command>
    </ExecuteCommand>
  </soap:Body>
</soap:Envelope>
```

The response carries an `<ok>` boolean and the command's `<output>` (XML-escaped):

```xml
<soap:Envelope xmlns:soap="http://schemas.xmlsoap.org/soap/envelope/">
  <soap:Body>
    <ExecuteCommandResponse>
      <ok>true</ok>
      <output>Server: RS2V Custom Server | Map: ... | Players: 3/64 | Tick: 60Hz</output>
    </ExecuteCommandResponse>
  </soap:Body>
</soap:Envelope>
```

A ready-to-use client lives at [`tools/soap_command.py`](../tools/soap_command.py):

```bash
python3 tools/soap_command.py --host 127.0.0.1 --port 27015 \
        --password a-strong-shared-secret "query"
```

**Security:** the request body is size-bounded (64 KB) and parsed defensively;
malformed/oversized input fails closed. The password is checked with a
length-independent comparison. The listener binds **all interfaces** (a
limitation of the socket layer) — you **must** firewall the port to trusted
hosts. Never enable this on an untrusted network without a firewall rule.

---

## 4 · Command Reference

Use `help` for the live, permission-filtered list and `help <command>` for usage.
Aliases are shown in parentheses.

### Automation / health
| Command | Level | Usage |
|---|---|---|
| `ping` | Player | Liveness probe; replies `pong`. |
| `version` (`ver`) | Player | Report server version. |
| `echo` | Player | Echo arguments back. |
| `query` (`q`) | Helper | Machine-readable snapshot (`key=value` lines). |
| `help` (`?`, `commands`) | Player | List permitted commands / show usage. |

### Server management
| Command | Level | Usage |
|---|---|---|
| `status` (`svstatus`) | Helper | Name, map, players, tick rate, time scale. |
| `players` (`playerlist`, `list`) | Helper | Connected players with id/SteamID/team/ping/K-D-score. |
| `reload` | Admin | Reload configuration from disk. |
| `tickrate` | Admin | `tickrate <hz>` — set tick rate (1-256). |
| `shutdown` (`quit`, `exit`) | Admin | Graceful shutdown. |

### Communication
| Command | Level | Usage |
|---|---|---|
| `broadcast` (`bc`, `say`) | Moderator | `broadcast <message...>` — server-wide announcement. |

### Player management
| Command | Level | Usage |
|---|---|---|
| `kick` | Moderator | `kick <steamId\|#id> [reason...]` |
| `ban` | Admin | `ban <steamId\|#id> <minutes\|permanent> [reason...]` |
| `unban` | Admin | `unban <steamId>` |
| `banlist` (`bans`) | Moderator | List active bans with remaining time. |
| `admins` (`adminlist`) | Helper | List configured admins and levels. |
| `slay` (`kill`) | Moderator | `slay <steamId\|#id>` — instantly kill. |
| `god` | Moderator | `god <steamId\|#id\|all> <on\|off>` — invincibility. |
| `tp` (`teleport`) | Moderator | `tp <steamId\|#id> <x> <y> <z>` |
| `give` | Moderator | `give <steamId\|#id\|all> <item> [amount]` |
| `team` | Moderator | `team <steamId\|#id> <teamId>` |

### Map management
| Command | Level | Usage |
|---|---|---|
| `changemap` (`map`) | Admin | `changemap <mapName>` |
| `maps` (`maplist`) | Helper | List configured maps (current marked `*`). |
| `rotation` | Admin | Show the configured map rotation. |
| `startvote` | Moderator | Start an end-of-round map vote. |

### Mods / workshop
| Command | Level | Usage |
|---|---|---|
| `mods` | Helper | List loaded mods/assets. |
| `mutators` | Helper | List active and available mutators. |
| `workshop` | Admin | `workshop [list\|reload\|validate\|download]` |

### Anti-cheat
| Command | Level | Usage |
|---|---|---|
| `eacmode` | Admin | `eacmode <safe\|emulate\|off>` — sets `EAC.mode` (applies on next EAC start). |

### Configuration
| Command | Level | Usage |
|---|---|---|
| `get` (`cvar_get`) | Admin | `get <Section.key>` |
| `set` (`cvar_set`) | Admin | `set <Section.key> <value>` (runtime; persist with `saveconfig`) |
| `saveconfig` (`save`) | Admin | Persist configuration to disk. |
| `config` | Admin | `config <get\|set\|save\|sections\|keys> [...]` |

### Dev / diagnostics
| Command | Level | Usage |
|---|---|---|
| `timescale` | Dev | `timescale <scale>` — sim speed 0.05-8.0 (1.0 = normal). |
| `regen` (`regenhandlers`) | Dev | Regenerate + reload packet handlers. |
| `spawnbot` (`bot`) | Dev | Spawn AI bots — **not implemented** (no bot subsystem yet). |

> Players may be referenced by **SteamID** (canonical) or by **`#<clientId>`**
> when they are connected.

---

## 5 · Admin List Management

`config/admin_list.txt` maps SteamIDs to permission levels. One entry per line:

```
# SteamID64          Level   Name/Comment
76561198000000001    3       Lead Admin
76561198000000002    2       Moderator - Alice
76561198000000004    1       Helper - Charlie
```

- The **level** column is the numeric tier from §2 (0-5).
- A bare SteamID with no level is treated as **Admin (3)** for backward
  compatibility with the legacy flat list.
- The list is loaded at startup.

### Bans

There is a **single** authoritative ban store, owned by the security layer
(`BanManager`, behind the login bridge) — the same store that enforces bans at
connect time. The `ban`/`unban`/`banlist` commands and anti-cheat all go through
it; `AdminManager` no longer keeps a separate ban list (a previous shadow list
drifted from and was clobbered by the security layer's).

Bans persist in `config/ban_list.txt`, pipe-delimited:

```
SteamID64|P|<unused>|reason             # permanent
SteamID64|T|<expiryUnixSeconds>|reason  # temporary (UTC seconds since epoch)
```

The file is machine-managed (rewritten on ban/unban/cleanup), so hand edits are
best made with the server stopped. Temporary-ban expiry is wall-clock, so bans
survive restarts.

---

## 6 · Automation Notes

The command system is designed to be driven by tooling and AI as well as humans:

- **`query`** returns a stable `key=value` snapshot (`name=`, `map=`, `players=`,
  `maxplayers=`, `tickrate=`, `timescale=`, `nonfatal_exceptions=`) — easy to
  parse without scraping prose. `nonfatal_exceptions` is the count of recovered
  (non-fatal) exceptions, useful for health monitoring.
- **`ping`** / **`echo`** give cheap liveness and round-trip checks.
- Remote SOAP responses always include a machine-readable `<ok>` boolean plus the
  full `<output>`. Remote requests run with `machine = true`, so handlers prefer
  terse output.
- Every command returns a success boolean, surfaced to remote callers as `<ok>`.

A typical automation loop: `query` for state → decide → issue a mutating command
(`changemap`, `tickrate`, `kick`, …) → re-`query` to confirm.

---

## 7 · Security Best Practices

1. **Keep the admin list minimal** — grant the lowest level each person needs.
2. **Never expose remote SOAP without a firewall** — it binds all interfaces; an
   open port + password on the public internet is a remote-control surface. Bind
   the host's firewall to trusted IPs and use a strong password.
3. **Use a strong `RemoteAdmin.password`** — it is the only auth factor for the
   remote transport.
4. **Audit log** — every accepted command is logged at INFO with transport,
   invoker, command, and args. Review it for misuse.
5. **Prefer temporary bans**; reserve `permanent` for egregious cases.
6. **`set` is runtime-only** until `saveconfig` — verify a change with `get`
   before persisting.

---

**End of ADMIN_COMMANDS.md**
