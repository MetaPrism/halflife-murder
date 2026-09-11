/*
===============================================================================
Crowbar Hunt loot: pickups that appear around the map during a live round.

Where they can appear comes from one of two places, and only ever one:

  - ch_loot_spawn point entities placed in the map by its author. If the map
    has any of these, they are the whole table and the file below is ignored.

  - maps/<mapname>_loot.txt, for maps nobody here can recompile. One "model
    <path>" line sets the model for every coordinate line after it, and every
    other line is "x y z". "//" comments and blank lines are ignored.

The gamerules own the table and decide when a spawn fires; this file knows how
to fill the table and what a piece of loot is.
===============================================================================
*/
#pragma once

#include "cbase.h"

constexpr int CH_MAX_LOOT_SPAWNS = 256;

struct CHLootSpawn
{
	// A string_t rather than a buffer: the engine's precache table keeps the
	// pointer it is given, so the model name has to outlive the load.
	string_t iszModel;
	Vector   vecOrigin;
	EHANDLE  hLoot; // the ch_loot sitting here right now, or null
};

// Fill pTable from maps/<mapname>_loot.txt and precache its models. Returns
// the number of entries, 0 if there is no file or nothing usable in it. Must
// run while precaching is still allowed - the gamerules constructor does.
int CH_LoadLootFile(CHLootSpawn* pTable, int maxCount);

// Fill pTable from the map's ch_loot_spawn entities. Returns 0 if the map has
// none, in which case the caller keeps whatever the file gave it. Only valid
// once the map has finished spawning its entities.
int CH_CollectLootSpawnEntities(CHLootSpawn* pTable, int maxCount);

// Put a piece of loot on the table entry's spot. Returns null if the spot is
// outside the world.
CBaseEntity* CH_CreateLoot(CHLootSpawn& spawn);
