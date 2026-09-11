#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "crowbar_hunt_loot.h"
#include "crowbar_hunt_gamerules.h"

// Stands in for a spot that was never given a model: a coordinate line before
// the file's first "model" line, or a ch_loot_spawn with the key left blank.
constexpr const char* CH_DEFAULT_LOOT_MODEL = "models/w_battery.mdl";

constexpr int CH_LOOT_LINE_MAX = 256;
constexpr int CH_LOOT_MODEL_MAX = 64;

// ---------------------------------------------------------------------------
// ch_loot_spawn - a map author's marker for where loot may appear. Invisible
// and inert in play; the gamerules read its position and model off it once,
// when the map has finished spawning.
// ---------------------------------------------------------------------------
class CCrowbarHuntLootSpawn : public CPointEntity
{
public:
	void Spawn() override;
	void Precache() override;
};

LINK_ENTITY_TO_CLASS(ch_loot_spawn, CCrowbarHuntLootSpawn);

void CCrowbarHuntLootSpawn::Precache()
{
	if (FStringNull(pev->model))
		pev->model = MAKE_STRING(CH_DEFAULT_LOOT_MODEL);

	PRECACHE_MODEL(STRING(pev->model));
}

void CCrowbarHuntLootSpawn::Spawn()
{
	Precache();
	CPointEntity::Spawn();
	pev->effects |= EF_NODRAW;
}

// ---------------------------------------------------------------------------
// ch_loot - one collectable piece, lying where a spawn put it until a player
// walks into it or the round resets and sweeps it up with the rest of the
// litter.
// ---------------------------------------------------------------------------
class CCrowbarHuntLoot : public CBaseEntity
{
public:
	void Spawn() override;
	int  ObjectCaps() override { return FCAP_DONT_SAVE; }
	void EXPORT LootTouch(CBaseEntity* pOther);
};

LINK_ENTITY_TO_CLASS(ch_loot, CCrowbarHuntLoot);

void CCrowbarHuntLoot::Spawn()
{
	SET_MODEL(ENT(pev), STRING(pev->model));

	pev->movetype = MOVETYPE_TOSS;
	pev->solid = SOLID_TRIGGER;
	UTIL_SetOrigin(pev, pev->origin);
	UTIL_SetSize(pev, Vector(-16, -16, 0), Vector(16, 16, 16));
	SetTouch(&CCrowbarHuntLoot::LootTouch);
	CH_SetWeaponGlow(this, CHWeaponGlow::Loot);

	// A coordinate read off "status" is the player's origin, which sits well
	// above their feet; dropping it puts the model on the floor.
	if (DROP_TO_FLOOR(ENT(pev)) == 0)
	{
		ALERT(at_console, "Crowbar Hunt: loot spawn at %.0f %.0f %.0f is outside the world\n", pev->origin.x, pev->origin.y, pev->origin.z);
		UTIL_Remove(this);
	}
}

void CCrowbarHuntLoot::LootTouch(CBaseEntity* pOther)
{
	if (!pOther->IsPlayer())
		return;

	CHalfLifeCrowbarHunt* pRules = CH_GetCrowbarHuntRules();

	if (!pRules || !pRules->CollectLoot(static_cast<CBasePlayer*>(pOther)))
		return;

	EMIT_SOUND(ENT(pev), CHAN_ITEM, "items/gunpickup2.wav", 1, ATTN_NORM);
	SetTouch(nullptr);
	UTIL_Remove(this);
}

CBaseEntity* CH_CreateLoot(CHLootSpawn& spawn)
{
	// Not CBaseEntity::Create(): that spawns before there is a chance to set
	// the model, and Spawn() needs it.
	CCrowbarHuntLoot* pLoot = GetClassPtr(static_cast<CCrowbarHuntLoot*>(nullptr));

	pLoot->pev->classname = MAKE_STRING("ch_loot");
	pLoot->pev->model = spawn.iszModel;
	pLoot->pev->origin = spawn.vecOrigin;

	DispatchSpawn(pLoot->edict());

	if ((pLoot->pev->flags & FL_KILLME) != 0)
		return nullptr;

	spawn.hLoot = pLoot;
	return pLoot;
}

// ---------------------------------------------------------------------------
// Filling the table
// ---------------------------------------------------------------------------
int CH_CollectLootSpawnEntities(CHLootSpawn* pTable, int maxCount)
{
	int count = 0;
	CBaseEntity* pEntity = nullptr;

	while ((pEntity = UTIL_FindEntityByClassname(pEntity, "ch_loot_spawn")) != nullptr)
	{
		if (count >= maxCount)
		{
			ALERT(at_console, "Crowbar Hunt: over %d ch_loot_spawn entities on this map, the rest are ignored\n", maxCount);
			break;
		}

		CHLootSpawn& spawn = pTable[count++];

		spawn.iszModel = pEntity->pev->model; // Precache() filled in the default
		spawn.vecOrigin = pEntity->pev->origin;
		spawn.hLoot = nullptr;
	}

	return count;
}

int CH_LoadLootFile(CHLootSpawn* pTable, int maxCount)
{
	char szPath[128];
	snprintf(szPath, sizeof(szPath), "maps/%s_loot.txt", STRING(gpGlobals->mapname));

	int   fileSize = 0;
	byte* pFile = LOAD_FILE_FOR_ME(szPath, &fileSize);

	if (!pFile)
		return 0;

	string_t iszModel = MAKE_STRING(CH_DEFAULT_LOOT_MODEL);
	bool bModelPrecached = false; // the default only costs a precache slot if a line actually uses it

	int count = 0;
	int lineNo = 0;
	int pos = 0;

	while (pos < fileSize)
	{
		char szLine[CH_LOOT_LINE_MAX];
		int  len = 0;
		lineNo++;

		while (pos < fileSize && pFile[pos] != '\n' && pFile[pos] != '\r' && pFile[pos] != '\0')
		{
			if (len < CH_LOOT_LINE_MAX - 1)
				szLine[len++] = static_cast<char>(pFile[pos]);
			pos++;
		}

		// Exactly one terminator, so the line count stays honest across blank
		// lines. A stray NUL counts as one, or the loop above never advances.
		if (pos < fileSize && pFile[pos] == '\r')
			pos++;
		if (pos < fileSize && (pFile[pos] == '\n' || pFile[pos] == '\0'))
			pos++;

		szLine[len] = '\0';

		const char* pszLine = szLine;
		while (*pszLine == ' ' || *pszLine == '\t')
			pszLine++;

		if (*pszLine == '\0' || (pszLine[0] == '/' && pszLine[1] == '/'))
			continue;

		if (0 == strncmp(pszLine, "model", 5) && (pszLine[5] == ' ' || pszLine[5] == '\t'))
		{
			char szModel[CH_LOOT_MODEL_MAX];

			if (sscanf(pszLine + 5, " %63s", szModel) == 1)
			{
				iszModel = ALLOC_STRING(szModel);
				bModelPrecached = false;
			}
			else
			{
				ALERT(at_console, "%s(%d): \"model\" with no path\n", szPath, lineNo);
			}
			continue;
		}

		float x, y, z;

		if (sscanf(pszLine, "%f %f %f", &x, &y, &z) != 3)
		{
			ALERT(at_console, "%s(%d): expected \"x y z\" or \"model <path>\", skipped\n", szPath, lineNo);
			continue;
		}

		if (count >= maxCount)
		{
			ALERT(at_console, "%s(%d): over %d loot spawns, the rest are ignored\n", szPath, lineNo, maxCount);
			break;
		}

		if (!bModelPrecached)
		{
			PRECACHE_MODEL(STRING(iszModel));
			bModelPrecached = true;
		}

		CHLootSpawn& spawn = pTable[count++];

		spawn.iszModel = iszModel;
		spawn.vecOrigin = Vector(x, y, z);
		spawn.hLoot = nullptr;
	}

	FREE_FILE(pFile);

	return count;
}
