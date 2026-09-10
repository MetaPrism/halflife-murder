#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "client.h"
#include "cdll_dll.h" // MAX_PLAYERS
#include "gamerules.h"
#include "game.h"
#include "ch_bots.h"

// A bot is nothing more than an engine fake client that the game DLL connects
// and spawns by hand: the engine hands back an edict, but it is up to us to run
// it through the same ClientConnect/ClientPutInServer path a real player takes.
// Bots send no usercmds, so BotThink() feeds them an empty move each frame to
// keep gravity, touch and the rest of player physics running on them.
//
// Which slots hold a bot is tracked here rather than read back off
// pev->flags, because a disconnected player's edict is deliberately left
// allocated (see ClientDisconnect) with its flags intact: a kicked bot still
// looks like a live FL_FAKECLIENT for the rest of the map, and calling
// pfnRunPlayerMove on a slot the engine no longer has a client in crashes the
// server.
static bool g_bIsBot[MAX_PLAYERS + 1] = {};

// The name each bot was created with. pev->netname can't answer that question:
// under ch_anonymous the gamerules overwrite both it and the userinfo name key
// with a disguise, so a slot already holding "Bot1" reads back as somebody
// else and the next bot picks the same name.
static char g_szBotName[MAX_PLAYERS + 1][32] = {};

static float g_flLastBotMoveTime = 0;

// Wander state, only used while bot_zombie is 0. A bot walks along one yaw
// until it stops making progress - it walked into a wall, a door or another
// player - and then picks a new one. Progress is sampled on an interval rather
// than per frame, because a single frame's worth of movement is too small to
// tell apart from being stuck.
static float g_flBotYaw[MAX_PLAYERS + 1] = {};
static Vector g_vecBotLastOrigin[MAX_PLAYERS + 1] = {};
static float g_flBotNextProgressCheck[MAX_PLAYERS + 1] = {};

// How often a bot's progress is sampled, and how far it has to have travelled
// in that time to count as still moving.
constexpr float BOT_PROGRESS_INTERVAL = 0.3f;
constexpr float BOT_PROGRESS_DISTANCE = 16.0f;

static void BotPickNewDirection(int index, CBaseEntity* pPlayer)
{
	g_flBotYaw[index] = RANDOM_FLOAT(-180, 180);
	g_vecBotLastOrigin[index] = pPlayer->pev->origin;
	g_flBotNextProgressCheck[index] = gpGlobals->time + BOT_PROGRESS_INTERVAL;
}

// Picks a name no connected player is already using, so the engine doesn't
// rename the fake client behind our back.
static void BotPickName(char* out, int outSize)
{
	for (int suffix = 1; suffix < 128; ++suffix)
	{
		char candidate[32];
		snprintf(candidate, sizeof(candidate), "Bot%d", suffix);

		bool taken = false;

		for (int i = 1; i <= gpGlobals->maxClients; ++i)
		{
			if (g_bIsBot[i] && FStrEq(g_szBotName[i], candidate))
			{
				taken = true;
				break;
			}

			CBaseEntity* pPlayer = UTIL_PlayerByIndex(i);

			if (pPlayer && FStrEq(STRING(pPlayer->pev->netname), candidate))
			{
				taken = true;
				break;
			}
		}

		if (!taken)
		{
			strncpy(out, candidate, outSize - 1);
			out[outSize - 1] = '\0';
			return;
		}
	}

	strncpy(out, "Bot", outSize - 1);
	out[outSize - 1] = '\0';
}

static void BotAdd()
{
	if (!g_pGameRules)
	{
		ALERT(at_console, "Can't add a bot: no map loaded\n");
		return;
	}

	char name[32];

	if (CMD_ARGC() > 1 && '\0' != *CMD_ARGV(1))
	{
		strncpy(name, CMD_ARGV(1), sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
	}
	else
	{
		BotPickName(name, sizeof(name));
	}

	edict_t* pEdict = g_engfuncs.pfnCreateFakeClient(name);

	if (FNullEnt(pEdict))
	{
		ALERT(at_console, "Can't add a bot: server is full\n");
		return;
	}

	const int index = ENTINDEX(pEdict);

	if (index < 1 || index > MAX_PLAYERS)
	{
		ALERT(at_console, "Can't add a bot: bad client slot %d\n", index);
		return;
	}

	char* infobuffer = g_engfuncs.pfnGetInfoKeyBuffer(pEdict);

	// The engine gives a fake client an empty userinfo; without a model and
	// colours other clients have nothing to draw it with.
	g_engfuncs.pfnSetClientKeyValue(index, infobuffer, "model", "gordon");
	g_engfuncs.pfnSetClientKeyValue(index, infobuffer, "topcolor", "30");
	g_engfuncs.pfnSetClientKeyValue(index, infobuffer, "bottomcolor", "6");

	char rejectReason[128] = "";

	if (0 == ClientConnect(pEdict, name, "127.0.0.1", rejectReason))
	{
		ALERT(at_console, "Can't add a bot: %s\n", rejectReason);
		SERVER_COMMAND(UTIL_VarArgs("kick # %d\n", GETPLAYERUSERID(pEdict)));
		return;
	}

	ClientPutInServer(pEdict);

	// Spawn() keeps FL_FAKECLIENT (player.cpp masks the flags), but set it
	// anyway so nothing downstream can mistake a bot for a real client.
	pEdict->v.flags |= FL_FAKECLIENT;

	g_bIsBot[index] = true;

	strncpy(g_szBotName[index], name, sizeof(g_szBotName[index]) - 1);
	g_szBotName[index][sizeof(g_szBotName[index]) - 1] = '\0';

	g_flBotYaw[index] = RANDOM_FLOAT(-180, 180);
	g_vecBotLastOrigin[index] = pEdict->v.origin;
	g_flBotNextProgressCheck[index] = gpGlobals->time + BOT_PROGRESS_INTERVAL;

	ALERT(at_console, "Added bot \"%s\"\n", name);
}

static void BotKickAll()
{
	int kicked = 0;

	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; ++i)
	{
		if (!g_bIsBot[i])
			continue;

		// kick is queued in the command buffer and runs after this frame, so
		// drop the slot now: nothing should touch a bot that is on its way out.
		g_bIsBot[i] = false;
		g_szBotName[i][0] = '\0';

		SERVER_COMMAND(UTIL_VarArgs("kick # %d\n", GETPLAYERUSERID(INDEXENT(i))));
		++kicked;
	}

	ALERT(at_console, "Kicked %d bot%s\n", kicked, 1 == kicked ? "" : "s");
}

void InitBotCommands()
{
	g_engfuncs.pfnAddServerCommand("bot", &BotAdd);
	g_engfuncs.pfnAddServerCommand("bot_kickall", &BotKickAll);
}

void BotClientDisconnected(edict_t* pEntity)
{
	const int index = ENTINDEX(pEntity);

	if (index >= 1 && index <= MAX_PLAYERS)
	{
		g_bIsBot[index] = false;
		g_szBotName[index][0] = '\0';
	}
}

void BotThink()
{
	const float flDelta = gpGlobals->time - g_flLastBotMoveTime;

	g_flLastBotMoveTime = gpGlobals->time;

	// Guard against the first frame of a level, where the delta is meaningless.
	if (flDelta <= 0 || flDelta > 1.0f)
		return;

	const byte msec = static_cast<byte>(V_max(1, V_min(255, static_cast<int>(flDelta * 1000.0f))));

	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; ++i)
	{
		if (!g_bIsBot[i])
			continue;

		CBaseEntity* pPlayer = UTIL_PlayerByIndex(i);

		if (!pPlayer || !FBitSet(pPlayer->pev->flags, FL_FAKECLIENT))
			continue;

		// A live bot with bot_zombie off walks: everything else gets an empty
		// move, which exists purely so the engine runs player physics on a
		// client that never sends usercmds.
		if (0 != bot_zombie.value || !pPlayer->IsAlive())
		{
			g_engfuncs.pfnRunPlayerMove(pPlayer->edict(), pPlayer->pev->v_angle, 0, 0, 0, 0, 0, msec);
			continue;
		}

		if (gpGlobals->time >= g_flBotNextProgressCheck[i])
		{
			if ((pPlayer->pev->origin - g_vecBotLastOrigin[i]).Length2D() < BOT_PROGRESS_DISTANCE)
			{
				BotPickNewDirection(i, pPlayer);
			}
			else
			{
				g_vecBotLastOrigin[i] = pPlayer->pev->origin;
				g_flBotNextProgressCheck[i] = gpGlobals->time + BOT_PROGRESS_INTERVAL;
			}
		}

		const Vector vecAngles(0, g_flBotYaw[i], 0);

		// Face where it is going, so other players see the bot turn.
		pPlayer->pev->angles = vecAngles;
		pPlayer->pev->v_angle = vecAngles;

		const float flSpeed = pPlayer->pev->maxspeed > 0 ? pPlayer->pev->maxspeed : 240.0f;

		g_engfuncs.pfnRunPlayerMove(pPlayer->edict(), vecAngles, flSpeed, 0, 0, 0, 0, msec);
	}
}
