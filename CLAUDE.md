# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this repository is

A fork of [Half-Life Updated](https://github.com/twhl-community/halflife-updated) (the TWHL community's bug-fixed Half-Life 1 SDK) carrying one mod on top: **Crowbar Hunt**, a round-based "one killer vs. the rest" multiplayer mode.

Everything outside `dlls/crowbar_hunt_gamerules.*` is upstream SDK code. Upstream's scope is deliberately narrow — bug fixes and code cleanup only, no gameplay changes, no engine or graphics changes. Mod work belongs in the Crowbar Hunt files and the seams they hook into; avoid reshaping upstream systems, since that makes future upstream merges painful.

`README.md`, `BUILDING.md`, and `INSTALL.md` are upstream's docs and describe the *base SDK*, not this mod.

## Building

Three build systems are maintained in parallel and all three must be kept in sync (see "Adding a source file").

**Windows (Visual Studio 2019+)** — `projects/vs2019/projects.sln` builds both DLLs. This is what CI runs:

```
MSBuild -property:Configuration=Release -target:Rebuild projects/vs2019/projects.sln
```

Output lands in `projects/vs2019/Release/hldll/hl.dll` and `projects/vs2019/Release/hl_cdll/client.dll`. **Must be built as x86/Win32** — the engine is 32-bit. `projects/vs2019/utils.sln` is a separate solution for the old Valve map/model tools in `utils/`; it is unrelated to gameplay work.

**Linux (i386)**:

```
cd linux && make CFG=release COMPILER=g++ -j2
```

Produces `linux/release/hl.so` and `linux/release/client.so`. Needs `g++-multilib` and 32-bit OpenGL headers. `CFG=debug` for a debug build.

**CMake** — supported but not the default path; `BUILDING.md` documents it. Requires the Win32 architecture on Windows and `cmake/LinuxToolchain.cmake` on Linux. `CMAKE_INSTALL_PREFIX` must point at your Half-Life mod directory; the `INSTALL` target then deploys the binaries plus `network/delta.lst`.

**Fast syntax check without a full build.** When you only need to know whether touched files compile, invoke `cl` directly with the project's own flags rather than building the solution:

```
call "<VS install>\VC\Auxiliary\Build\vcvars32.bat"
cl /nologo /c /Zs /EHsc /std:c++17 /W3 ^
  /D WIN32 /D _CRT_SECURE_NO_WARNINGS /D NDEBUG /D _WINDOWS /D QUIVER /D VOXEL /D QUAKE2 /D VALVE_DLL /D CLIENT_WEAPONS ^
  /I dlls /I engine /I common /I pm_shared /I game_shared /I public ^
  dlls\<file>.cpp
```

Use `/D CLIENT_DLL /D HL_DLL` instead of `/D VALVE_DLL` to check a file as the client build sees it.

There is no test suite. CI (`.github/workflows/ci-cd.yml`) only builds: Linux i386 under clang and gcc, Linux under the Steam Runtime scout container, and Win32 via MSBuild.

## Architecture

**Two DLLs, one source tree.** `hl.dll` (from `dlls/`) is server-side game logic; `client.dll` (from `cl_dll/`) is HUD, VGUI, and client-side rendering/prediction. `common/`, `engine/`, `public/`, `pm_shared/` (player movement), and `game_shared/` are shared headers linked into both. The engine loads both through the C exports in `dlls/h_export.cpp` and `cl_dll/cdll_int.cpp`.

**Weapon code is compiled into both DLLs.** `cl_dll/CMakeLists.txt` (and the client `.vcxproj`) pull `../dlls/crowbar.cpp`, `python.cpp`, `weapons_shared.cpp`, and friends into the *client* build with `CLIENT_DLL` defined, so the client can predict firing locally. Consequences when editing any weapon file:

- Code guarded by `#ifndef CLIENT_DLL` is server-only; unguarded code runs in both, where server-only globals and entity APIs do not exist.
- A change to firing behaviour usually needs a matching change to the client-side event in `cl_dll/ev_hldm.cpp`, or client and server predictions desync.
- Adding a weapon source file means adding it to *both* DLLs' build entries.

**Gamerules.** `CGameRules` in `dlls/gamerules.h` is the mode interface: round flow, spawning, damage, respawn policy, scoring. `CHalfLifeMultiplay` is the multiplayer base most modes derive from. `InstallGameRules()` at the bottom of `dlls/gamerules.cpp` picks the concrete class once at map load, dispatching on server cvars — that function is the single entry point for making a new mode reachable. Mode-selection cvars are declared in `dlls/game.h`, then defined and `CVAR_REGISTER`ed in `dlls/game.cpp`; `sv_busters` is the reference example.

**Entities** are C++ classes bound to map classnames via `LINK_ENTITY_TO_CLASS`, with save/restore driven by static `TYPEDESCRIPTION m_SaveData[]` tables and `Save`/`Restore` overrides (`dlls/cbase.h`). Any new persistent member on an entity needs a `m_SaveData` entry or it will not survive a save/level change.

**Server→client messaging.** Custom HUD data goes through user messages: the `gmsg*` handles in `dlls/UserMessages.h`, registered server-side, sent with `MESSAGE_BEGIN`/`WRITE_*`, and hooked client-side in `cl_dll/hud_msg.cpp` / `hud.cpp`. `network/delta.lst` defines the networked entity/player field deltas and must be shipped to the mod directory alongside the DLLs when it changes.

## Fork-specific gotchas

Half-Life Updated diverges from the vanilla HLSDK that most Half-Life tutorials, snippets, and AI-generated code assume. Verify every SDK symbol against this tree before trusting it:

- **`bool`, not `BOOL`.** Upstream reworked int/BOOL-typed booleans to real `bool` throughout, including virtuals on `CGameRules` and `CBaseEntity`. Pasted vanilla code declaring `BOOL FPlayerCanRespawn(...) override` will fail to compile. Likewise `RemoveAllItems(false)`, not `FALSE`.
- **Headers moved.** There is no `multiplay_gamerules.h`; `CHalfLifeMultiplay` lives in `gamerules.h`.
- **`util.h` is not the vanilla one.** Several commonly cited helpers do not exist (e.g. there is no `UTIL_PlayersInGame`). Iterate players with `UTIL_PlayerByIndex(i)` over `1..gpGlobals->maxClients`.

`dlls/README.md` is the original Crowbar Hunt integration note and repeats this warning; treat its instructions as historical — steps 1 and 2 are already done.

## Adding a source file

A new file under `dlls/` must be registered in **all** of:

- `dlls/CMakeLists.txt` (separate source and header lists)
- `projects/vs2019/hldll.vcxproj` *and* `hldll.vcxproj.filters` (`ClCompile` and `ClInclude` entries in each)
- `linux/Makefile.hldll` (`$(HLDLL_OBJ_DIR)/<name>.o \` in the object list)

Client-side files go through the `cl_dll/` equivalents instead. A file that compiles but was only added to CMake will silently be missing from the CI and Visual Studio builds.

## Conventions

- `.clang-format` (LLVM base, real tabs, width 4, no column limit) and `.clang-tidy` are checked in; match surrounding style.
- `.gitattributes` pins line endings per file type — CRLF for `*.bat`, `*.sln`, `*.vcxproj`, `*.vcxproj.filters`; LF for `Makefile*` and `*.sh`. The `.vcxproj` files are UTF-8 **with BOM**. Preserve both when editing build files programmatically; a tool that normalizes them will produce a noisy diff.
- `filecopy.bat` is the Visual Studio post-build step that copies binaries into a mod directory; the hardcoded `mod_directory` path near the top is meant to be edited locally.

## Crowbar Hunt: current state

`CHalfLifeCrowbarHunt` (`dlls/crowbar_hunt_gamerules.h` / `.cpp`) derives from `CHalfLifeMultiplay` and runs a `WaitingForPlayers → PreRound → InProgress → RoundEnd` state machine. One player is the Killer (crowbar only), one is a secret Hunter (`weapon_357`), the rest are unarmed Survivors. Roles are stored in a `CHRole m_playerRoles[MAX_PLAYERS + 1]` array indexed by `ENTINDEX()`.

It is wired into all three build systems and selectable at map load via `sv_crowbarhunt 1` (deathmatch only). The round loop is implemented: role assignment, loadouts, private per-player role announcements, observer-on-death, forced respawns between rounds, and win conditions. Late joiners are left `Unassigned` and sit out the current round as observers rather than counting as extra Survivors.

It has not been playtested in a running server yet. Known open items: spawn-point separation (the Killer can spawn next to the Hunter, since `GetPlayerSpawnSpot` is not overridden), balance tuning, and the optional role/round HUD element in `cl_dll/` described as step 4 of `dlls/README.md`.

Between rounds the map is restored in place rather than by restarting the level. `ResetMapEntities()` replays a snapshot (taken on the first `Think()`, before anything can be broken or opened) over every tracked func_breakable/pushable/door/button: entvars state plus `m_pfnThink`/`m_pfnTouch`/`m_pfnUse`, which the break and door-move code rewires as it runs. It also sweeps gibs, weaponboxes and live ordnance, and deletes loose `weapon_*`/`ammo_*` map pickups (map guns are deliberately out of play). This relies on one upstream hook: `CGameRules::ShouldPreserveBrokenEntities()`, which `CBreakable::Die()` checks to keep a broken brush's edict allocated - it must clear `FL_KILLME` as well as skip `SUB_Remove`, since `CBaseEntity::Killed()` already flagged the entity for the engine to free. Decals still persist; nothing short of a map change clears them.

Two ordering constraints in this file are easy to break:

- `CBasePlayer::Killed()` calls `PlayerKilled()` *before* it sets `deadflag`, so the victim already reads as not-`IsAlive()` (health is `<= 0`) and win-condition counts are correct there — but calling `StartObserver()` at that point would be undone by the rest of `Killed()`. The observer transition is deferred to `Think()`, which waits for `deadflag == DEAD_DEAD`.
- `CBasePlayer::Spawn()` resets `m_afPhysicsFlags` but *not* `pev->iuser1`, so force-respawning an observer must clear `iuser1`/`iuser2` afterwards, the way `ClientPutInServer()` does.
