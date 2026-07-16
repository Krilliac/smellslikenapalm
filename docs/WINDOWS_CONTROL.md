# Windows server and retail-client control

Run the emulator from the repository root with Windows PowerShell 5.1 or newer:

```powershell
powershell -NoProfile -File .\tools\server_control.ps1 -Action Start
powershell -NoProfile -File .\tools\server_control.ps1 -Action Status
powershell -NoProfile -File .\tools\server_control.ps1 -Action Logs -TailLines 80
powershell -NoProfile -File .\tools\server_control.ps1 -Action Watch -TailLines 20
powershell -NoProfile -File .\tools\server_control.ps1 -Action Restart
powershell -NoProfile -File .\tools\server_control.ps1 -Action Stop
```

To smoke-test another configured map without rewriting `config/server.ini`, pass
the startup-only map override:

```powershell
powershell -NoProfile -File .\tools\server_control.ps1 -Action Restart -Map VNTE-CuChi
```

`-Port 17777` and `-EacPort 17957` likewise apply no-config-edit startup
overrides. Use a distinct EAC port when running an isolated second instance; the
game and EAC ports are intentionally independent and are never silently derived
from one another. `-Map`, `-Port`, and `-EacPort` are scoped to that controller
invocation. A later scheduled `Ensure` uses its own/default arguments, so
configure the scheduled invocation explicitly if an override must survive an
unexpected process exit.

Server-control launches default to `-ReplicationBootstrapVariant installed`,
which gives the child process `RS2V_REPLICATION_BOOTSTRAP_VARIANT=installed`.
Pass `-ReplicationBootstrapVariant canonical` to exercise the canonical
no-override path; in that mode the variable is omitted from the child rather
than set to the nonempty value `canonical`. The controller restores its own
process environment immediately after creating the child.

All normal canonical and installed launches use the live per-session actor
cohort. `RS2V_REPLAY_CAPTURE_WORLD=1` is a direct, startup-only reverse-engineering
diagnostic for the canonical Resort/Territories capture; it fails closed with the
controller's default installed artifact or any other map/profile. It is not a
hot-reloadable server setting.

`Stop` is deliberately a hard process termination. The retail client can remain stuck
in pretransition after a graceful network disconnect, so the controller kills the
exact process whose PID, executable path, and creation time are recorded in
`.server-control/state.json`. It will not kill an unowned same-name process.

An explicit `Stop` writes `.server-control/disabled` before inspecting the process.
The scheduled-task wrapper `tools/ensure_server.ps1` honors that sentinel and will not
immediately relaunch the server. `Start` and `Restart` remove the sentinel; `Ensure`
does not. Lifecycle operations are serialized across interactive and scheduled-task
Windows sessions, while `Status`, `Logs`, and `Watch` remain read-only and do not hold
the lifecycle lock.

Sentinel removal is committed only by a successful startup. If process launch,
executable-path verification, state publication, or the startup grace probe fails,
the controller kills only its returned child handle, removes any published state, and
restores `.server-control/disabled` before returning the error. This prevents a failed
manual start from leaving scheduled `Ensure` enabled with no managed process.

The default binary search order is:

1. `build-merge\<BuildConfig>\rs2v_server.exe`
2. `build\<BuildConfig>\rs2v_server.exe`
3. `out\build\x64-Debug\rs2v_server.exe` for Debug builds

Use `-Binary` or `-Config` for another file inside the repository. Paths containing
spaces are supported. Repository escapes and reparse-point paths are rejected.

Active stdout and stderr are under `.server-control`. Each start rotates the previous
pair into `.server-control/logs`, retaining 20 sessions. `Watch` follows both active
logs and reports lifecycle changes; press Ctrl+C to stop watching. Watching does not
prevent another shell from stopping or restarting the server.

The following checks do not stop, restart, or signal the live server:

```powershell
powershell -NoProfile -File .\tools\server_control.ps1 -Action SelfTest
powershell -NoProfile -File .\tools\test_server_control.ps1
```

The first validates the current repository paths, argument quoting, and managed-state
identity. The second copies the scripts into a temporary repository whose path contains
spaces, then exercises missing/corrupt/stale state, the explicit-stop sentinel, wrapper
exit codes, and bounded log tails without launching a server.

## Retail client control

`tools/client_control.ps1` launches the retail client through Steam app 418460. It
never starts `VNGame.exe` directly; direct execution does not establish the Steam
context required by the retail build.

```powershell
powershell -NoProfile -File .\tools\client_control.ps1 -Action Status
powershell -NoProfile -File .\tools\client_control.ps1 -Action Start
powershell -NoProfile -File .\tools\client_control.ps1 -Action Connect
powershell -NoProfile -File .\tools\client_control.ps1 -Action Recover
powershell -NoProfile -File .\tools\client_control.ps1 -Action Restart
powershell -NoProfile -File .\tools\client_control.ps1 -Action Stop
```

`Start` resolves `steam.exe` from a running Steam process, the Valve registry
entries, or the normal Program Files locations. `-SteamPath` can select another
regular file named `steam.exe`. Steam receives `-applaunch 418460`; the controller
then waits for the resulting `VNGame.exe` instead of treating the Steam launcher
process as the game.

Each launch includes a random, inert `-RS2VClientControl=<guid>` argument. A client
becomes managed only when all of the following match:

1. no other `VNGame.exe` is observed before or during launch;
2. the new process command line contains that launch token;
3. its exact executable path, PID, and creation FILETIME are readable; and
4. those values are atomically recorded in `.client-control/state.json` as UTF-8
   without a byte-order mark.

The EAC-protected retail process can hide its executable path and command line from
CIM and `.NET Process.Path`. The controller falls back to Windows
`PROCESS_QUERY_LIMITED_INFORMATION` queries for those two read-only identity values;
quoted token arguments are parsed as exact arguments, not substring matches. If a
client appears but its ownership proof is still unavailable after a three-second
identity grace period, launch fails promptly and leaves that client unmanaged instead
of waiting for the full launch window.

The launch window is `-LaunchTimeoutSeconds` (120 seconds by default) plus
`-LateLaunchGraceSeconds` (30 seconds by default), for a default total of 150 seconds.
The late-launch grace keeps observing for a tagged `VNGame.exe` when Steam creates it
just after the base timeout. It can be set from 0 through 60 seconds; zero disables
the extra window. This is separate from the three-second identity grace above, which
starts only after a client process has appeared but its executable identity, command
line, or token cannot yet be verified. For example, this allows up to 150 seconds for
Steam to create the tagged client:

```powershell
powershell -NoProfile -File .\tools\client_control.ps1 -Action Connect `
  -LaunchTimeoutSeconds 120 -LateLaunchGraceSeconds 30
```

Lifecycle actions use an app-global mutex, so controllers in different repository
copies cannot race a machine-global Steam launch. `Stop` is deliberately a hard
termination, matching the server workflow needed to
escape the retail client's stuck pretransition state. It re-verifies the token,
path, PID, and creation time immediately before termination. An unmanaged client,
inaccessible process identity, corrupt state, PID reuse, or missing token fails
closed; the script will not guess which game process belongs to it. `Status` may
report unmanaged clients but remains read-only.

### Recovering a late tagged client

`Recover` repairs controller ownership when Steam started the controller-tagged
retail client too late for the launch invocation to publish state, or when the old
state is stale. It does not launch, stop, or signal the game:

```powershell
powershell -NoProfile -File .\tools\client_control.ps1 -Action Status
powershell -NoProfile -File .\tools\client_control.ps1 -Action Recover
```

Recovery is intentionally strict. Existing state must be missing or stale (an
already-managed client is simply reported as managed), and exactly one `VNGame.exe`
may be running. Its executable path, creation FILETIME, and command line must all be
readable. The executable must be named `VNGame.exe` and end in the normal Steam
layout `\steamapps\common\Rising Storm 2\Binaries\Win64\VNGame.exe`. Its command
line must contain exactly one well-formed, nonzero
`-RS2VClientControl=<guid>` argument; substrings, missing tokens, and duplicate tokens
are rejected.

The controller refuses recovery when there are zero or multiple client candidates,
the process identity is inaccessible, the executable is outside that Steam layout,
the token proof is absent or ambiguous, or existing state is corrupt,
identity-mismatched, or otherwise indeterminate. After writing recovered state it
immediately re-verifies every identity component. A failed verification removes only
the matching newly written state and leaves the process untouched.

### Direct connect

When the client is stopped, `Connect` appends the UE3 server URL to the Steam launch:

```powershell
powershell -NoProfile -File .\tools\client_control.ps1 -Action Connect `
  -ServerAddress 127.0.0.1 -ServerPort 7777
```

Loopback is the default safety boundary. A numeric non-loopback address requires
the explicit `-AllowRemoteAddress` switch. Host names other than `localhost`,
quotes, whitespace, control characters, invalid IP literals, and ports outside
1-65535 are rejected before Steam is invoked.

Steam cannot safely inject a new UE3 URL into an already-running client. In that
case `Connect` leaves the process untouched and prints the exact in-game command to
use, for example `open 127.0.0.1:7777`. Stop/relaunch with `Connect` when automated
connection is required.

The controller calls `steam.exe -applaunch` directly; it does not use a
`steam://run/...` URI. Steam URI launches with custom arguments can display a
Continue/Cancel confirmation. If the installed Steam client also asks for
confirmation on direct `-applaunch`, accept it within the combined
`-LaunchTimeoutSeconds` and `-LateLaunchGraceSeconds` window, or use plain `Start`
followed by the printed/manual in-game `open` command. The controller does not
synthesize input into Steam dialogs.

### Client-control validation

These checks never launch, stop, or signal the retail client:

```powershell
powershell -NoProfile -File .\tools\client_control.ps1 -Action SelfTest
powershell -NoProfile -File .\tools\test_client_control.ps1
```

The deeper smoke test uses a temporary `steam.exe` and a uniquely named mock
client to exercise the full lifecycle and direct-connect argument flow:

```powershell
powershell -NoProfile -File .\tools\tests\client_control_smoke.ps1
```

It generates a per-run process name and rewrites only its temporary controller
copy to discover that unique mock;
the production `VNGame.exe` is outside the test's process filter. This keeps the
default test path from overlapping with or controlling a live game session.
