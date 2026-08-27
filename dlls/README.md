# Crowbar Hunt - integration notes

These two files are a **skeleton**, not a finished mode. Function names
(`RemoveAllItems`, `GiveAmmo`, `ENTINDEX`, `UTIL_PlayersInGame`,
`HUD_PRINTCENTER`, ammo constants like `_357_MAX_CARRY`, etc.) follow the
traditional HLSDK `CGameRules` interface. Check them against this repo's
actual `dlls/gamerules.h`, `dlls/player.h`, and `dlls/weapons.h` before it
will compile - names sometimes drift slightly between SDK forks.

## 1. Add the files to the project
Drop `crowbar_hunt_gamerules.h` / `.cpp` into `dlls/`, then add them to
whatever build system this repo uses (CMakeLists.txt, or the .vcxproj on
Windows) alongside the existing `multiplay_gamerules.cpp`.

## 2. Hook it into gamerules selection
Somewhere in the codebase (commonly `dlls/game.cpp` or `dlls/gamerules.cpp`)
there's a function like `InstallGameRules()` that does something like:

```cpp
if (teamplay.value)
    g_pGameRules = new CHalfLifeTeamplay();
else if (deathmatch.value)
    g_pGameRules = new CHalfLifeMultiplay();
else
    g_pGameRules = new CHalfLifeRules();
```

Search the repo for `new CHalfLifeMultiplay` to find this spot, then add a
branch (e.g. gated on a new `mp_crowbarhunt` cvar) that does
`g_pGameRules = new CHalfLifeCrowbarHunt();` instead.

## 3. Fill in the TODOs
The biggest gaps to close, roughly in the order you'll hit them:

- **`CountAlivePlayersWithRole` / `PickRandomAlivePlayer`** - these are
  stubbed out. You need a real "loop over connected players" helper; look
  at how `multiplay_gamerules.cpp` already iterates players (e.g. for
  intermission or scoreboard code) and copy that pattern.
- **Spawn point handling** - vanilla HL just uses `info_player_deathmatch`
  spawns. You may want separate spawn logic so the Killer doesn't spawn
  next to the Hunter.
- **Observer/spectator on death** - right now dead players just sit there
  per `FPlayerCanRespawn` returning false. Look at how spectator mode is
  triggered elsewhere in `player.cpp` (search for `observer` or
  `IN_ATTACK`/`+attack` spectate binds) and call that from `PlayerKilled`.
- **Private role announcement** - `AssignRoles()` currently only has a
  TODO for telling the Hunter their role privately; use `ClientPrint`
  targeted at that one player's `edict_t*`, not `UTIL_ClientPrintAll`.
- **Balance** - crowbar vs. revolver is inherently asymmetric; you'll
  probably want to tune the Killer's speed, the crowbar's swing rate, or
  the revolver's damage/ammo count once you can playtest.

## 4. Client-side (optional, for round/role HUD)
If you want an on-screen "You are the Killer" banner or a round timer HUD
element instead of `HUD_PRINTCENTER` text, that lives in `cl_dll/` -
`hud.cpp` and `hud_msg.cpp` are the usual entry points for adding a new
custom HUD element driven by a `MessageBegin`/`WriteByte` message sent
from the server side of your gamerules class.
