# ersc-fix

A companion fix for **Elden Ring Seamless Co-op**. It runs alongside the mod and corrects a set of
co-op desync bugs. The mod itself is never modified; the fix hooks behaviour at runtime from its own DLL.

This is largely vibe-coded.

## What it fixes

- **Invisible Torrent.** A partner's horse stays invisible after they arrive mounted, or after you fast-travel while they ride.
- **Partner frozen, sliding or invisible after their death.** Your copy of them is revived once they respawn.
- **Partner away at the Roundtable** left standing in the ground, or shown nearby instead of far away.
- **World state after a death reload.** Event flags are guarded and handed over from the other player's world, enemies the other game had disabled stay disabled
- **Missing spirit-ash summons.** A partner's summon announced far away is re-delivered so it appears.
- **Crash or freeze while a player is loading,** caused by the two games endlessly re-sending a map-load flag to each other. The loop is detected and cut.
- **Crash on quit** doesn't meaningfully affect anything, except generating false crash reports

Each fix can be switched off in `Game\SeamlessCoop\fix\ersc_fix.ini` (created on first run; set a key to `0`).

## Requirements

- Windows, Steam copy of Elden Ring (`eldenring.exe` 2.7.2).
- Seamless Co-op v2.0.2 installed and working.
- Co-op only: the fix is loaded only inside Seamless Co-op sessions and never touches vanilla online play.
- PvE only: this is only tested with PvE
- Vanilla only: this is only tested with the base eldenring game

On any other game or mod version the fix refuses to start and says so, rather than running broken. Use
the normal `ersc_launcher.exe` until an updated release exists.

## Install

**Every player in the session needs the fix installed** - it works between players who both run it; a player without it plays stock Seamless Co-op.

1. Download the latest zip from the **Releases** page of this repository.
2. Right-click the zip → Properties → **Unblock** (Windows marks downloaded files), then extract it into `ELDEN RING\Game\`. This creates `Game\SeamlessCoop\fix\`.
3. Start the game with `Game\SeamlessCoop\fix\ersc_fix_launcher.exe`. It launches Seamless Co-op as usual and adds the fix. Started any other way, the game runs without it.

Logs: `Game\SeamlessCoop\ersc_fix_logs\`. Uninstall: delete `Game\SeamlessCoop\fix\`.

## If Windows blocks it

The launcher and DLL are unsigned, so Windows may refuse to run or load them. The launcher tells you when
the DLL could not be loaded; the usual causes and fixes:

- **SmartScreen** ("Windows protected your PC"): click *More info* → *Run anyway*. Unblocking the zip before extracting (step 2) avoids this.
- **Smart App Control** (Windows 11, enforced mode) blocks unsigned code outright. Turn it off under *Windows Security → App & browser control → Smart App Control settings*. Note Windows does not let you turn it back on without reinstalling.
- **Microsoft Defender / antivirus** may quarantine the DLL as an injector. Restore it and add `Game\SeamlessCoop\fix\` as an exclusion.

## Build from source

Needs Visual Studio 2022 (Desktop C++ workload), CMake 3.25+, and internet access for the first
configure (it fetches [safetyhook](https://github.com/cursey/safetyhook)).

```powershell
cmake --preset x64
cmake --build --preset release
```

The build also writes `build/ersc-fix-<version>.zip`, the same zip that is published on the Releases page:
extract it into `ELDEN RING/Game/` and start the game through the launcher as above. (The raw binaries are in
`build/bin/Release/`.)
