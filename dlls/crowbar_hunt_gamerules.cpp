#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "client.h"
#include "skill.h"
#include "game.h"
#include "hltv.h"
#include "shake.h"
#include "crowbar_hunt_gamerules.h"
#include "crowbar_hunt_shared.h"
#include "UserMessages.h"

// Minimum number of connected players before a round will start.
// TODO: expose as a cvar once there's a reason to tune it per server.
constexpr int CH_MIN_PLAYERS = 2;

// Both role weapons are one-hit kills: the round is decided by positioning and
// who shoots first, not by trading damage.
constexpr float CH_CROWBAR_DAMAGE = 100.0f;
constexpr float CH_357_DAMAGE = 100.0f;

// How far a voice carries, in world units, while ch_proxvoice is on. Around a
// large room: close enough that hiding still works, far enough that two people
// in the same corridor can talk.
// TODO: expose as a cvar if this needs tuning per server.
constexpr float CH_PROXVOICE_RADIUS = 800.0f;

// The shape of the misfire penalty; its length is ch_punish_time. Barely more
// than a walk and a jump too short to clear anything, so a shooter who guessed
// wrong is left where they stand for whoever comes looking.
constexpr float CH_PUNISH_SPEED = 100.0f;

// Percentage of the normal jump height, handed to the client as a physinfo
// string because PM_Jump() has to read the same number the server does.
constexpr const char* CH_PUNISH_JUMP_PERCENT = "50";
constexpr const char* CH_NORMAL_JUMP_PERCENT = "100";

// Seconds the punishment tint takes to arrive and to lift. Fading in fast keeps
// it legible as a consequence of the shot; lifting slowly stops the moment the
// penalty ends from reading as a graphical glitch.
constexpr float CH_PUNISH_TINT_FADE_IN = 0.4f;
constexpr float CH_PUNISH_TINT_FADE_OUT = 1.5f;

// How long after a player spawns to send them the scoreboard title. Long enough
// that a player spawning during the initial level load has a client DLL with its
// user message hooks installed by the time it arrives.
constexpr float CH_SERVERNAME_SEND_DELAY = 2.0f;

// Where anonymous mode reads its names from, relative to the mod directory:
// one name per line, blank lines and // comments ignored. Keeping the list out
// of the binary lets a server set its own flavour without a rebuild.
constexpr const char* CH_ANON_NAME_FILE = "ch_anonnames.txt";

constexpr int CH_MAX_ANON_NAMES = 64;

namespace
{
// Used when the names file is missing or has nothing usable in it. Deliberately
// flat: a name that reads as a joke says something about whoever is wearing it,
// which is the one thing anonymous mode exists to stop.
const char* const g_szDefaultAnonNames[] = {
	"Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf", "Hotel",
	"India", "Juliet", "Kilo", "Lima", "Mike", "November", "Oscar", "Papa",
};

char g_szAnonNames[CH_MAX_ANON_NAMES][CH_MAX_ANON_NAME];
int  g_numAnonNames = 0;

// Fisher-Yates over 0..count-1, so a deal can walk the result and hand out
// each entry once.
void ShuffledOrder(int* pOrder, int count)
{
	for (int i = 0; i < count; i++)
		pOrder[i] = i;

	for (int i = count - 1; i > 0; i--)
	{
		const int j = RANDOM_LONG(0, i);
		const int tmp = pOrder[i];
		pOrder[i] = pOrder[j];
		pOrder[j] = tmp;
	}
}

// Map entities whose spawn state is snapshotted and restored between rounds.
// Everything here is either a brush that can be destroyed or one that can be
// left sitting in the wrong position (open door, pushed-in button).
const char* const g_szResettableClassnames[] = {
	"func_breakable",
	"func_pushable",
	"func_door",
	"func_door_rotating",
	"func_water",
	"func_button",
	"func_rot_button",
	"momentary_door",
	"momentary_rot_button",
	"func_wall_toggle",
};

// Leftovers from the previous round: gibs, dropped weapon bags, live ordnance
// and in-flight projectiles.
const char* const g_szLitterClassnames[] = {
	"gib",
	"ch_corpse",
	"ch_loot",
	"weaponbox",
	"grenade",
	"crowbar_thrown",
	"monster_satchel",
	"monster_tripmine",
	"monster_snark",
	"rpg_rocket",
	"crossbow_bolt",
	"hornet",
	"spark_shower",
	"beam",
	"laser_spot",
};

// Map pickups other than guns and ammo that are kept out of play.
const char* const g_szMapItemClassnames[] = {
	"item_battery",
	"item_longjump",
	"item_healthkit",
};

template <int SIZE>
bool ClassnameInList(const char* pszClassname, const char* const (&list)[SIZE])
{
	for (int i = 0; i < SIZE; i++)
	{
		if (0 == strcmp(pszClassname, list[i]))
			return true;
	}

	return false;
}

// A player's chosen colours live in their userinfo, not on their entity. Read
// one out and clamp it into the byte the studio renderer's remap expects.
//
// 0..255 is the whole colour wheel, not degrees - the engine spreads it over
// all 360 - so this clamp costs nothing: every colour a player can pick fits
// in the byte a corpse's colormap carries. StudioDrawPlayer() clamps to 360
// instead, which is just the engine's own range check showing through.
int PlayerRemapColor(CBasePlayer* pPlayer, const char* pszKey)
{
	const int color = atoi(g_engfuncs.pfnInfoKeyValue(g_engfuncs.pfnGetInfoKeyBuffer(pPlayer->edict()), pszKey));

	if (color < 0)
		return 0;

	if (color > 255)
		return 255;

	return color;
}
} // namespace

// A dead player's body, left lying where they fell until the round resets.
//
// Half-Life's own bodyque cannot do this job: it is a ring of four slots that
// older deaths get recycled out of, and its corpses draw with
// kRenderFxDeadPlayer, which copies model and animation off the *live* player
// entity at that index every frame. A dead player here becomes an observer -
// EF_NODRAW, modelindex 0, no longer transmitted by AddToFullPack() - so there
// is nothing left for that corpse to copy and it renders as nothing. This one
// owns its model outright and does not care what the player does afterwards.
class CCrowbarHuntCorpse : public CBaseEntity
{
public:
	int ObjectCaps() override { return FCAP_DONT_SAVE; }
};

LINK_ENTITY_TO_CLASS(ch_corpse, CCrowbarHuntCorpse);

// The instance InstallGameRules() built, for the server commands below. Only
// ever compared against g_pGameRules, never dereferenced blind: a mode change
// leaves this pointing at rules the engine has already dropped.
namespace
{
CHalfLifeCrowbarHunt* g_pCrowbarHuntRules = nullptr;
}

CHalfLifeCrowbarHunt* CH_GetCrowbarHuntRules()
{
	return g_pCrowbarHuntRules == g_pGameRules ? g_pCrowbarHuntRules : nullptr;
}

// "ch_odds" - print each player's current share of the Killer draw.
//
// Server console only, deliberately: the weights are a record of who has been
// the Killer lately, which is exactly what anonymous mode exists to hide. A
// version of this a player could type would hand the room last round's answer.
static void CH_PrintKillerOdds()
{
	if (!g_pCrowbarHuntRules || g_pCrowbarHuntRules != g_pGameRules)
	{
		ALERT(at_console, "ch_odds: not running Crowbar Hunt\n");
		return;
	}

	ALERT(at_console, "Killer draw odds (weight x%.2f decay, +%.2f recover per round):\n",
		ch_killer_decay.value, ch_killer_recover.value);

	int shown = 0;

	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; i++)
	{
		CBasePlayer* pPlayer = CHalfLifeCrowbarHunt::GetPlayerByIndexPublic(i);

		if (!pPlayer)
			continue;

		const float flChance = g_pCrowbarHuntRules->GetKillerChancePercent(i);
		const float flWeight = g_pCrowbarHuntRules->GetKillerWeight(i);

		// A connected player who is not in the draw right now (dead, or a
		// mid-round joiner sitting it out) still has a weight worth seeing.
		ALERT(at_console, "  %-24s  %5.1f%%  (weight %.2f)%s\n",
			STRING(pPlayer->pev->netname), flChance, flWeight,
			flChance > 0.0f ? "" : "  [not in draw]");

		++shown;
	}

	if (0 == shown)
		ALERT(at_console, "  (nobody connected)\n");
}

// "ch_loot_respawn" - put a piece of loot on every empty spot, for testing a
// map's loot table without waiting on the interval. Server console only, like
// the rest: a player who could fire it would be handing themselves the round.
static void CH_LootRespawn()
{
	if (!g_pCrowbarHuntRules || g_pCrowbarHuntRules != g_pGameRules)
	{
		ALERT(at_console, "ch_loot_respawn: not running Crowbar Hunt\n");
		return;
	}

	const int placed = g_pCrowbarHuntRules->SpawnAllLoot();
	ALERT(at_console, "ch_loot_respawn: placed %d piece%s of loot\n", placed, placed == 1 ? "" : "s");
}

// "ch_loot_list" - print the loot table as loaded: every spot, its model, and
// whether a piece is sitting on it right now.
static void CH_LootList()
{
	if (!g_pCrowbarHuntRules || g_pCrowbarHuntRules != g_pGameRules)
	{
		ALERT(at_console, "ch_loot_list: not running Crowbar Hunt\n");
		return;
	}

	g_pCrowbarHuntRules->PrintLootTable();
}

void InitCrowbarHuntCommands()
{
	g_engfuncs.pfnAddServerCommand("ch_odds", &CH_PrintKillerOdds);
	g_engfuncs.pfnAddServerCommand("ch_loot_respawn", &CH_LootRespawn);
	g_engfuncs.pfnAddServerCommand("ch_loot_list", &CH_LootList);
}

CHalfLifeCrowbarHunt::CHalfLifeCrowbarHunt()
{
	g_pCrowbarHuntRules = this;

	m_roundState        = CHRoundState::WaitingForPlayers;
	m_flStateEnterTime  = gpGlobals->time;
	m_flPreRoundLength  = 5.0f;
	m_flRoundEndLength  = 8.0f;
	m_flRoundTimeLimit  = 0.0f;
	m_numSnapshots      = 0;
	m_bSnapshotTaken    = false;

	// From here, inside CWorld::Precache(), so the file's models can still be
	// precached. The map's own markers are picked up later - see Think().
	m_numLootSpawns       = CH_LoadLootFile(m_lootSpawns, CH_MAX_LOOT_SPAWNS);
	m_bLootSpawnsResolved = false;
	m_flNextLootSpawn     = 0.0f;
	m_bGrantingKillerGun  = false;

	// Announce straight away the first time we tick in WaitingForPlayers.
	m_flNextWaitingAnnounce = 0.0f;

	m_flRoundOverAnnounceTime = 0.0f;
	m_szRoundOverMessage[0]   = '\0';
	PRECACHE_SOUND(CH_ROUND_OVER_SOUND);

	m_bAnonActive = false;

	for (int i = 0; i <= MAX_PLAYERS; i++)
		ResetPlayerSlot(i);

	// CHalfLifeMultiplay's constructor already called RefreshSkillData(), but it
	// did so while the object was still a CHalfLifeMultiplay - virtual dispatch
	// during base construction never reaches a derived override - so our damage
	// values have to be applied again from here.
	RefreshSkillData();
}

// ---------------------------------------------------------------------------
// Called once per server frame (via g_pGameRules->Think() in the engine loop)
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::Think()
{
	// Let the base class keep handling generic multiplayer bookkeeping.
	// Remove this call if it fights with your own round timer/HUD messages.
	CHalfLifeMultiplay::Think();

	EnforceSpeedCeiling();

	// First frame after the map finished spawning its entities: record what
	// everything looked like before anyone can break or open it, and clear the
	// map's guns out straight away.
	if (!m_bSnapshotTaken)
		ResetMapEntities();

	// Same moment, same reason: the map's ch_loot_spawn markers exist now.
	if (!m_bLootSpawnsResolved)
		ResolveLootSpawns();

	// A round needs bodies in it. If players drop out mid-round and take us
	// below the minimum, abort straight back to WaitingForPlayers instead of
	// letting the state machine play out a round nobody can win - otherwise a
	// lone player keeps cycling through PreRound/RoundEnd on their own.
	if (m_roundState != CHRoundState::WaitingForPlayers && CountConnectedPlayers() < CH_MIN_PLAYERS)
	{
		AbortRound();
		return;
	}

	switch (m_roundState)
	{
	case CHRoundState::WaitingForPlayers:
		if (CountConnectedPlayers() >= CH_MIN_PLAYERS)
		{
			StartPreRound();
		}
		else if (gpGlobals->time >= m_flNextWaitingAnnounce)
		{
			AnnounceWaitingForPlayers();
			m_flNextWaitingAnnounce = gpGlobals->time + CH_WAITING_ANNOUNCE_INTERVAL;
		}
		break;

	case CHRoundState::PreRound:
		if (gpGlobals->time - m_flStateEnterTime >= m_flPreRoundLength)
			StartRound();
		break;

	case CHRoundState::InProgress:
		// Park anyone who died this frame in observer mode, then see whether
		// that death decided the round.
		MoveDeadPlayersToObserver();
		CheckRoundWinConditions();

		if (m_roundState == CHRoundState::InProgress)
			ServiceLootSpawns();

		// The Killer ran out the clock: everyone still standing has survived.
		// After the win check, so a kill that lands on the final frame still
		// counts the way it would have a moment earlier.
		if (m_roundState == CHRoundState::InProgress &&
			m_flRoundTimeLimit != 0.0f && gpGlobals->time >= m_flRoundTimeLimit)
		{
			EndRound(CHRole::Survivor, "Time's up! The Survivors win the round!\n");
		}
		break;

	case CHRoundState::RoundEnd:
		// Keep parking the dead in observer mode - the death that ended the
		// round happens before its animation finishes, so the last victim
		// still needs picking up. ResetForNextRound() takes them back out.
		MoveDeadPlayersToObserver();

		if (m_flRoundOverAnnounceTime != 0.0f && gpGlobals->time >= m_flRoundOverAnnounceTime)
			AnnounceRoundOver();

		if (gpGlobals->time - m_flStateEnterTime >= m_flRoundEndLength)
			ResetForNextRound();
		break;
	}
}

void CHalfLifeCrowbarHunt::SetRoundState(CHRoundState state)
{
	// The waiting notice is kept alive by re-sends, so it has to be taken
	// down explicitly when we stop waiting.
	if (m_roundState == CHRoundState::WaitingForPlayers && state != CHRoundState::WaitingForPlayers)
		ClearWaitingForPlayers();

	// A result still waiting to go up belongs to the round we're leaving.
	if (state != CHRoundState::RoundEnd)
		m_flRoundOverAnnounceTime = 0.0f;

	m_roundState       = state;
	m_flStateEnterTime = gpGlobals->time;

	// The clock only runs while a round is live. Starting one winds it up;
	// leaving InProgress by any route - a win, a timeout, an abort - takes it
	// off everyone's HUD.
	if (state == CHRoundState::InProgress && ch_round_time.value > 0.0f)
	{
		m_flRoundTimeLimit = gpGlobals->time + ch_round_time.value;
		SendRoundTimer(nullptr);
	}
	else if (m_flRoundTimeLimit != 0.0f)
	{
		m_flRoundTimeLimit = 0.0f;
		SendRoundTimer(nullptr);
	}

	// Entering the waiting state should say so immediately, not up to a full
	// interval later.
	if (state == CHRoundState::WaitingForPlayers)
		m_flNextWaitingAnnounce = gpGlobals->time;
}

void CHalfLifeCrowbarHunt::StartPreRound()
{
	SetRoundState(CHRoundState::PreRound);
	AssignRoles();

	// Put the world back before anyone is placed in it: broken crates return,
	// doors close, and the previous round's debris is swept up.
	ResetMapEntities();

	// Put everyone back on a spawn point for the countdown. PlayerSpawn()
	// withholds weapons while we're not InProgress, so they stand around
	// unarmed until StartRound() hands out the loadouts.
	ForceRespawnAllPlayers();

	// After the respawns, not before: CHalfLifeMultiplay::PlayerSpawn() hands a
	// crowbar and glock to every spawning player, so the wipe has to come last.
	StripAllPlayers();

	UTIL_ClientPrintAll(HUD_PRINTCENTER, "Roles assigned. Get ready...\n");
}

void CHalfLifeCrowbarHunt::StartRound()
{
	SetRoundState(CHRoundState::InProgress);

	// The first piece of loot arrives one interval in, not at the whistle.
	m_flNextLootSpawn = gpGlobals->time + ch_loot_interval.value;

	// Hand out weapons the instant the round goes live rather than at spawn
	// time, so nobody is armed during the countdown.
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (!pPlayer || !pPlayer->IsAlive())
			continue;

		const CHRole role = GetPlayerRole(pPlayer);

		GiveRoleLoadout(pPlayer, role);
		AnnounceRole(pPlayer, role);
		SendRoleHud(pPlayer);
	}

	//i believe this line is overriding the client-side announcement
	//UTIL_ClientPrintAll(HUD_PRINTCENTER, "Round started! Survive... or hunt.\n");
}

// pszMessage replaces the stock result line when the round ended some way
// other than the last death - the clock running out, say.
//
// The result is not shown here: it is held for CH_ROUND_OVER_DELAY and put up
// by AnnounceRoundOver() from Think(), so the kill that decided the round has
// a moment to itself first.
void CHalfLifeCrowbarHunt::EndRound(CHRole winningRole, const char* pszMessage)
{
	SetRoundState(CHRoundState::RoundEnd);

	const char* msg = "Round over.";
	switch (winningRole)
	{
	case CHRole::Killer:
		msg = "The Killer wins the round!";
		break;
	case CHRole::Hunter:
	case CHRole::Survivor:
		msg = "The Survivors win the round!";
		break;
	default:
		break;
	}

	strncpy(m_szRoundOverMessage, pszMessage ? pszMessage : msg, sizeof(m_szRoundOverMessage) - 1);
	m_szRoundOverMessage[sizeof(m_szRoundOverMessage) - 1] = '\0';

	// After SetRoundState(), which clears any pending announcement.
	m_flRoundOverAnnounceTime = gpGlobals->time + CH_ROUND_OVER_DELAY;
}

// The delayed round result: the text EndRound() stored, as a HUD message on
// its own channel, with CH_ROUND_OVER_SOUND played to everyone at the same
// moment. Held for the rest of the RoundEnd state, so it drops off as the
// next round's reset arrives.
void CHalfLifeCrowbarHunt::AnnounceRoundOver()
{
	m_flRoundOverAnnounceTime = 0.0f;

	hudtextparms_t parms;
	memset(&parms, 0, sizeof(parms));

	parms.x      = -1.0f;
	parms.y      = 0.7f;
	parms.effect = 2;

	parms.r1 = 255;
	parms.g1 = 255;
	parms.b1 = 255;
	parms.a1 = 255;

	parms.r2 = 255;
	parms.g2 = 255;
	parms.b2 = 255;
	parms.a2 = 255;

	parms.fadeinTime  = 0.05f;
	parms.fadeoutTime = 1.0f;
	parms.holdTime    = V_max(1.0f, m_flRoundEndLength - CH_ROUND_OVER_DELAY - 1.0f);
	parms.fxTime      = 0.25f;

	// Channels 1 and 2 are the role reveal and the waiting notice.
	parms.channel = 3;

	UTIL_HudMessageAll(parms, m_szRoundOverMessage);

	// Full volume for everyone, wherever they are; CHAN_STATIC keeps it clear
	// of whatever the player's own weapon or voice channels are doing.
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);
		if (pPlayer)
			EMIT_SOUND(ENT(pPlayer->pev), CHAN_STATIC, CH_ROUND_OVER_SOUND, 1.0f, ATTN_NONE);
	}
}

void CHalfLifeCrowbarHunt::SendRoundTimer(edict_t* pTarget) const
{
	int seconds = 0;
	if (m_flRoundTimeLimit != 0.0f)
		seconds = V_max(0, static_cast<int>(ceilf(m_flRoundTimeLimit - gpGlobals->time)));

	if (pTarget)
		MESSAGE_BEGIN(MSG_ONE, gmsgCHTimer, nullptr, pTarget);
	else
		MESSAGE_BEGIN(MSG_ALL, gmsgCHTimer, nullptr);
	WRITE_SHORT(seconds);
	MESSAGE_END();
}

// The top-of-HUD role label. Roles are dealt in PreRound but only revealed at
// StartRound(), so the label follows the reveal: it shows from the moment the
// round goes live until ResetForNextRound() wipes the roles, and never for a
// sit-out or a mid-round joiner, who have no part in the round to show.
void CHalfLifeCrowbarHunt::SendRoleHud(CBasePlayer* pPlayer) const
{
	if (!pPlayer)
		return;

	int value = 0;
	if (m_roundState == CHRoundState::InProgress || m_roundState == CHRoundState::RoundEnd)
	{
		switch (GetPlayerRole(pPlayer))
		{
		case CHRole::Killer:
			value = 1;
			break;
		case CHRole::Hunter:
			value = 2;
			break;
		case CHRole::Survivor:
			value = 3;
			break;
		default:
			break;
		}
	}

	MESSAGE_BEGIN(MSG_ONE, gmsgCHRole, nullptr, pPlayer->edict());
	WRITE_BYTE(value);
	MESSAGE_END();
}

// Persistent notice, on its own HUD channel so it never collides with the
// round-result or role announcements. Think() re-sends it every
// CH_WAITING_ANNOUNCE_INTERVAL; the engine replaces a channel's message in
// place, and the hold outlasts the interval, so it reads as one steady
// message that stays up until ClearWaitingForPlayers().
void CHalfLifeCrowbarHunt::AnnounceWaitingForPlayers() const
{
	char szText[128];
	snprintf(szText, sizeof(szText), "Waiting for players...\n%d of %d connected",
		CountConnectedPlayers(), CH_MIN_PLAYERS);

	hudtextparms_t parms;
	memset(&parms, 0, sizeof(parms));

	parms.x      = -1.0f; // centred horizontally
	parms.y      = 0.7f;
	parms.effect = 0; // plain fade; a scan effect would replay on every re-send

	parms.r1 = 200;
	parms.g1 = 200;
	parms.b1 = 200;
	parms.a1 = 255;

	parms.r2 = 255;
	parms.g2 = 255;
	parms.b2 = 255;
	parms.a2 = 255;

	// No fades: the re-send lands while the previous copy is still fully
	// held, so it swaps without a visible blink.
	parms.fadeinTime  = 0.0f;
	parms.fadeoutTime = 0.0f;
	parms.holdTime    = CH_WAITING_ANNOUNCE_INTERVAL + 1.0f;
	parms.fxTime      = 0.0f;

	parms.channel = 2;

	UTIL_HudMessageAll(parms, szText);
}

// Replaces the waiting notice with a blank that expires immediately.
void CHalfLifeCrowbarHunt::ClearWaitingForPlayers() const
{
	hudtextparms_t parms;
	memset(&parms, 0, sizeof(parms));

	parms.x       = -1.0f;
	parms.y       = 0.7f;
	parms.channel = 2;

	UTIL_HudMessageAll(parms, " ");
}

// Called from Think() the moment the server drops below CH_MIN_PLAYERS while a
// round is running. Skips the RoundEnd hold entirely - there is no result to
// show - and hands straight back to WaitingForPlayers, which announces itself.
void CHalfLifeCrowbarHunt::AbortRound()
{
	UTIL_ClientPrintAll(HUD_PRINTCENTER, "Not enough players. Round aborted.\n");
	ResetForNextRound();
}

void CHalfLifeCrowbarHunt::ResetForNextRound()
{
	// Spectator survives the wipe - see the same exception in AssignRoles().
	for (int i = 0; i <= MAX_PLAYERS; i++)
	{
		if (!IsSittingOut(i))
			m_playerRoles[i] = CHRole::Unassigned;
	}

	ClearPunishments();

	SetRoundState(CHRoundState::WaitingForPlayers);

	// Roles are gone, so are the labels.
	for (int i = 1; i <= gpGlobals->maxClients; i++)
		SendRoleHud(GetPlayerByIndex(i));

	// Only the dead need picking up here - it gets them out of observer mode
	// so they aren't stuck spectating if the server drops below the player
	// minimum. StartPreRound() repositions everybody once a round can start.
	ForceRespawnDeadPlayers();
	StripAllPlayers();
}

// ---------------------------------------------------------------------------
// Map reset
//
// A round-based mode wants the map back the way it loaded, but a real level
// restart would drop every client into a loading screen between rounds. So
// instead we snapshot the state of the entities that can be left changed and
// restore that snapshot at the start of each round.
//
// Two things make this work: ShouldPreserveBrokenEntities() stops
// CBreakable::Die() from freeing broken brushes, so their edicts are still
// around to restore, and m_pfnThink/Touch/Use are saved alongside entvars,
// since Die() and the door/button move code rewire those as they go.
//
// Not covered: decals (blood, bullet holes) are client-side and can only be
// cleared by a real map change, and plats/trains/rotating brushes are left
// alone - add their classnames to g_szResettableClassnames if a map needs it.
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::TakeMapSnapshot()
{
	m_numSnapshots   = 0;
	m_bSnapshotTaken = true;

	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++)
	{
		edict_t* pEdict = INDEXENT(i);

		if (!pEdict || 0 != pEdict->free || FStringNull(pEdict->v.classname))
			continue;

		if (!ClassnameInList(STRING(pEdict->v.classname), g_szResettableClassnames))
			continue;

		CBaseEntity* pEntity = CBaseEntity::Instance(pEdict);

		if (!pEntity)
			continue;

		if (m_numSnapshots >= CH_MAX_TRACKED_ENTITIES)
		{
			ALERT(at_console, "Crowbar Hunt: over %d resettable entities on this map, the rest will not reset between rounds\n", CH_MAX_TRACKED_ENTITIES);
			break;
		}

		CHEntitySnapshot& snapshot = m_mapSnapshot[m_numSnapshots++];

		snapshot.hEntity       = pEntity;
		snapshot.vecOrigin     = pEntity->pev->origin;
		snapshot.vecAngles     = pEntity->pev->angles;
		snapshot.flHealth      = pEntity->pev->health;
		snapshot.flTakeDamage  = pEntity->pev->takedamage;
		snapshot.flFrame       = pEntity->pev->frame;
		snapshot.iSolid        = pEntity->pev->solid;
		snapshot.iMoveType     = pEntity->pev->movetype;
		snapshot.iEffects      = pEntity->pev->effects;
		snapshot.iszTargetName = pEntity->pev->targetname;
		snapshot.pfnThink      = pEntity->m_pfnThink;
		snapshot.pfnTouch      = pEntity->m_pfnTouch;
		snapshot.pfnUse        = pEntity->m_pfnUse;

		CBaseToggle* pToggle  = pEntity->MyTogglePointer();
		snapshot.iToggleState = pToggle ? pToggle->m_toggle_state : -1;
	}
}

void CHalfLifeCrowbarHunt::ResetMapEntities()
{
	if (!m_bSnapshotTaken)
		TakeMapSnapshot();

	RemoveRoundLitter();

	// Decals (blood, bullet holes) are client-side only and nothing the server
	// does to the map touches them, so tell every client to wipe its own.
	// (The engine's r_cleardecals command would do it too, but the Half-Life
	// engine build doesn't have it - it's a Counter-Strike addition.)
	MESSAGE_BEGIN(MSG_ALL, gmsgCHClearFX, nullptr);
	MESSAGE_END();

	for (int i = 0; i < m_numSnapshots; i++)
	{
		CHEntitySnapshot& snapshot = m_mapSnapshot[i];
		CBaseEntity*      pEntity  = snapshot.hEntity;

		if (!pEntity)
			continue;

		entvars_t* pev = pEntity->pev;

		pEntity->m_pfnThink = snapshot.pfnThink;
		pEntity->m_pfnTouch = snapshot.pfnTouch;
		pEntity->m_pfnUse   = snapshot.pfnUse;

		ClearBits(pev->flags, FL_KILLME); // in case something flagged it for the engine to free
		pev->deadflag   = DEAD_NO;
		pev->health     = snapshot.flHealth;
		pev->takedamage = snapshot.flTakeDamage;
		pev->effects    = snapshot.iEffects; // clears the EF_NODRAW a break left behind
		pev->frame      = snapshot.flFrame;
		pev->targetname = snapshot.iszTargetName; // Die() clears this so a breakable cannot retrigger itself
		pev->solid      = snapshot.iSolid;
		pev->movetype   = snapshot.iMoveType;
		pev->velocity   = g_vecZero;
		pev->avelocity  = g_vecZero;
		pev->angles     = snapshot.vecAngles;

		// Re-setting the brush model restores mins/maxs/size and relinks the
		// entity, which is what brings a broken brush back as a solid again.
		if (!FStringNull(pev->model))
			SET_MODEL(ENT(pev), STRING(pev->model));

		UTIL_SetOrigin(pev, snapshot.vecOrigin);

		CBaseToggle* pToggle = pEntity->MyTogglePointer();

		if (pToggle && snapshot.iToggleState >= 0)
		{
			pToggle->m_toggle_state       = static_cast<TOGGLE_STATE>(snapshot.iToggleState);
			pToggle->m_flActivateFinished = 0;
		}

		// Anything caught mid-move is parked here. Only entities that had a
		// think scheduled at spawn time (a sparking button, say) get one back;
		// those reschedule themselves from then on. Push movers run their
		// thinks off ltime, everything else off game time.
		if (pEntity->m_pfnThink)
			pev->nextthink = (pev->movetype == MOVETYPE_PUSH ? pev->ltime : gpGlobals->time) + 0.1;
		else
			pev->nextthink = 0;
	}
}

// Sweep up what the last round left lying around, and keep the map's own guns,
// ammo, batteries, long jump modules and medkits out of play - a Survivor picking up an
// MP5 would undo the whole premise.
void CHalfLifeCrowbarHunt::RemoveRoundLitter()
{
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++)
	{
		edict_t* pEdict = INDEXENT(i);

		if (!pEdict || 0 != pEdict->free || FStringNull(pEdict->v.classname))
			continue;

		const char* pszClassname = STRING(pEdict->v.classname);

		// The engine's four bodyque slots are permanent entities that cannot be
		// removed, and PlayerDeathThink() has already copied a (currently
		// invisible) body into one for every death this round. Blank them, or a
		// stale slot starts drawing again the moment its player respawns.
		if (0 == strcmp(pszClassname, "bodyque"))
		{
			pEdict->v.modelindex = 0;
			pEdict->v.effects |= EF_NODRAW;
			continue;
		}

		// A weapon or ammo entity someone is carrying has its owner set, so
		// only the ones lying in the world get taken.
		const bool isLooseWeapon = FNullEnt(pEdict->v.owner) && (0 == strncmp(pszClassname, "weapon_", 7) || 0 == strncmp(pszClassname, "ammo_", 5));

		// Batteries, long jump modules and health kits go the same way: armour
		// would blunt the crowbar, a long jump outruns it, and with one-hit
		// kills a medkit is nothing but a decoy - loot is the only pickup.
		const bool isMapItem = ClassnameInList(pszClassname, g_szMapItemClassnames);

		if (!isLooseWeapon && !isMapItem && !ClassnameInList(pszClassname, g_szLitterClassnames))
			continue;

		CBaseEntity* pEntity = CBaseEntity::Instance(pEdict);

		if (pEntity)
			UTIL_Remove(pEntity);
	}
}

// ---------------------------------------------------------------------------
// Loot
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::ResolveLootSpawns()
{
	m_bLootSpawnsResolved = true;

	const int numFromMap = CH_CollectLootSpawnEntities(m_lootSpawns, CH_MAX_LOOT_SPAWNS);

	if (numFromMap > 0)
	{
		if (m_numLootSpawns > 0)
			ALERT(at_console, "Crowbar Hunt: map has its own ch_loot_spawn markers, ignoring the loot file\n");

		m_numLootSpawns = numFromMap;
	}

	ALERT(at_console, "Crowbar Hunt: %d loot spawn%s on this map\n", m_numLootSpawns, m_numLootSpawns == 1 ? "" : "s");
}

void CHalfLifeCrowbarHunt::ServiceLootSpawns()
{
	if (ch_loot_interval.value <= 0.0f || m_numLootSpawns == 0)
		return;

	if (gpGlobals->time < m_flNextLootSpawn)
		return;

	m_flNextLootSpawn = gpGlobals->time + ch_loot_interval.value;

	// The empty spots, and how much is already lying around. A handle goes
	// null on its own when the piece is picked up or swept.
	int freeSpots[CH_MAX_LOOT_SPAWNS];
	int numFree = 0;
	int numLive = 0;

	for (int i = 0; i < m_numLootSpawns; i++)
	{
		if (static_cast<CBaseEntity*>(m_lootSpawns[i].hLoot) != nullptr)
			numLive++;
		else
			freeSpots[numFree++] = i;
	}

	if (numFree == 0 || numLive >= static_cast<int>(ch_loot_max.value))
		return;

	CH_CreateLoot(m_lootSpawns[freeSpots[RANDOM_LONG(0, numFree - 1)]]);
}

int CHalfLifeCrowbarHunt::SpawnAllLoot()
{
	// The table normally resolves on the first Think(); a command typed before
	// that (from a listen server's console, say) should still see the map's
	// markers.
	if (!m_bLootSpawnsResolved)
		ResolveLootSpawns();

	int placed = 0;

	for (int i = 0; i < m_numLootSpawns; i++)
	{
		if (static_cast<CBaseEntity*>(m_lootSpawns[i].hLoot) != nullptr)
			continue;

		if (CH_CreateLoot(m_lootSpawns[i]))
			placed++;
	}

	return placed;
}

void CHalfLifeCrowbarHunt::PrintLootTable()
{
	if (!m_bLootSpawnsResolved)
		ResolveLootSpawns();

	if (m_numLootSpawns == 0)
	{
		ALERT(at_console, "No loot spawns on this map (no ch_loot_spawn entities and no maps/%s_loot.txt)\n", STRING(gpGlobals->mapname));
		return;
	}

	ALERT(at_console, "%d loot spawn%s:\n", m_numLootSpawns, m_numLootSpawns == 1 ? "" : "s");

	for (int i = 0; i < m_numLootSpawns; i++)
	{
		const CHLootSpawn& spawn = m_lootSpawns[i];
		const bool occupied = static_cast<CBaseEntity*>(m_lootSpawns[i].hLoot) != nullptr;

		ALERT(at_console, "  %3d  %8.1f %8.1f %8.1f  %s%s\n", i + 1,
			spawn.vecOrigin.x, spawn.vecOrigin.y, spawn.vecOrigin.z,
			STRING(spawn.iszModel), occupied ? "  [loot present]" : "");
	}
}

bool CHalfLifeCrowbarHunt::CollectLoot(CBasePlayer* pPlayer)
{
	if (m_roundState != CHRoundState::InProgress || !pPlayer->IsAlive())
		return false;

	const CHRole role = GetPlayerRole(pPlayer);

	if (role == CHRole::Unassigned || role == CHRole::Spectator)
		return false;

	const int index = ENTINDEX(pPlayer->edict());

	m_iLootCount[index]++;
	SendLootCount(pPlayer);

	const int reward = static_cast<int>(ch_loot_reward.value);

	if (reward <= 0)
		return true;

	// Survivors buy a revolver every ch_loot_reward pieces. The Killer, if the
	// server allows it at all, buys one only - at double the price.
	int threshold;

	if (GetPlayerRole(pPlayer) == CHRole::Killer)
		threshold = (ch_loot_killer_gun.value != 0 && m_iLootRewards[index] == 0) ? reward * 2 : 0;
	else
		threshold = (m_iLootRewards[index] + 1) * reward;

	if (threshold > 0 && m_iLootCount[index] >= threshold)
	{
		m_iLootRewards[index]++;
		AwardLootRevolver(pPlayer);
	}

	return true;
}

void CHalfLifeCrowbarHunt::AwardLootRevolver(CBasePlayer* pPlayer)
{
	// The Killer's one purchase goes straight into their hands: CollectLoot()
	// only sends them here with ch_loot_killer_gun on, and the role gate in
	// CanHavePlayerItem() is opened for exactly this call. Never the floor -
	// a gun beside the Killer is a gun for whoever they are chasing.
	if (GetPlayerRole(pPlayer) == CHRole::Killer)
	{
		m_bGrantingKillerGun = true;
		pPlayer->GiveNamedItem("weapon_357");
		m_bGrantingKillerGun = false;

		ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "Your loot bought a revolver.\n");
		return;
	}

	// Into the hands if they are empty and allowed to fill; a punished shooter
	// is barred from rearming, so theirs goes on the floor for the duration.
	if (!pPlayer->HasNamedPlayerItem("weapon_357") && !IsPunished(pPlayer))
	{
		pPlayer->GiveNamedItem("weapon_357");
		ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "Your loot bought a revolver.\n");
		return;
	}

	CBaseEntity* pGun = CBaseEntity::Create("weapon_357", pPlayer->pev->origin, pPlayer->pev->angles);

	if (!pGun)
		return;

	// A map weapon respawns twenty seconds after it is taken; this one is a
	// one-off, the same as the dead Hunter's.
	pGun->pev->spawnflags |= SF_NORESPAWN;
	CH_SetWeaponGlow(pGun, CHWeaponGlow::Revolver);

	ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "Your loot bought a revolver - it's at your feet.\n");
}

void CHalfLifeCrowbarHunt::SendLootCount(CBasePlayer* pPlayer) const
{
	MESSAGE_BEGIN(MSG_ONE, gmsgCHLoot, nullptr, pPlayer->edict());
	WRITE_SHORT(m_iLootCount[ENTINDEX(pPlayer->edict())]);
	MESSAGE_END();
}

void CHalfLifeCrowbarHunt::ClearLootCounts()
{
	for (int i = 0; i <= MAX_PLAYERS; i++)
	{
		m_iLootCount[i] = 0;
		m_iLootRewards[i] = 0;
	}

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (pPlayer)
			SendLootCount(pPlayer);
	}
}

// ---------------------------------------------------------------------------
// Role assignment
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::AssignRoles()
{
	// Everyone starts Unassigned so that players who connect mid-round stay
	// Unassigned - CountAlivePlayersWithRole() ignores them, and PlayerSpawn()
	// drops them into observer mode until the next round.
	//
	// Spectator is the one role a deal does not clear: it is a standing request
	// to be left out, not a state belonging to the round being dealt. Wiping it
	// here is what used to hand a spectator a Survivor role again below.
	for (int i = 0; i <= MAX_PLAYERS; i++)
	{
		if (!IsSittingOut(i))
			m_playerRoles[i] = CHRole::Unassigned;
	}

	// A penalty belongs to the round it was earned in - carrying one over would
	// hand somebody a crippled Killer through no fault of their own.
	ClearPunishments();

	// So does a loot haul.
	ClearLootCounts();

	// New round, new faces. Deliberately not done in ResetForNextRound(): the
	// disguises stay on between rounds so nobody's real name flashes up on the
	// scoreboard in the gap.
	AssignAnonIdentities();

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (!pPlayer || IsSittingOut(i))
			continue;

		m_playerRoles[i] = CHRole::Survivor;
	}

	// Weighted, not flat: whoever was Killer recently is drawn less often, so
	// the role moves around the server instead of landing on the same player
	// three rounds running. See AgeKillerWeights() for the bookkeeping.
	CBasePlayer* pKiller = PickWeightedKiller();
	if (pKiller)
		SetPlayerRole(pKiller, CHRole::Killer);

	// Age the weights against this draw before the Hunter pick, which does not
	// touch them - only the Killer role is rationed.
	AgeKillerWeights(pKiller);

	// Pick the Hunter from everyone except whoever just got Killer.
	CBasePlayer* pHunter = PickRandomAlivePlayer(CHRole::Killer);
	if (pHunter)
		SetPlayerRole(pHunter, CHRole::Hunter);
}

// Tell one player what they are. Deliberately never broadcast - half the fun
// is the Killer not knowing who's carrying the revolver.
//
// Presented like a single-player chapter title: the same CHudMessage renderer
// env_message drives, with the write-out scan effect low on the screen.
// UTIL_HudMessage carries the text and its fade parameters inline in a
// TE_TEXTMESSAGE, so this needs no titles.txt entry and no client.dll change.
void CHalfLifeCrowbarHunt::AnnounceRole(CBasePlayer* pPlayer, CHRole role) const
{
	if (!pPlayer)
		return;

	const char* pszText = nullptr;
	byte r = 255, g = 255, b = 255;

	switch (role)
	{
	case CHRole::Killer:
		pszText = "YOU ARE THE KILLER\nHunt them down.";
		r = 200;
		g = 40;
		b = 40;
		break;

	case CHRole::Hunter:
		pszText = "YOU ARE THE HUNTER\nYou have the revolver - find the Killer.";
		r = 220;
		g = 170;
		b = 40;
		break;

	case CHRole::Survivor:
		pszText = "YOU ARE A SURVIVOR\nStay alive.";
		r = 200;
		g = 200;
		b = 200;
		break;

	default:
		return;
	}

	hudtextparms_t parms;
	memset(&parms, 0, sizeof(parms));

	parms.x = -1.0f;  // -1 centres the line horizontally
	parms.y = 0.7f;   // low on the screen, where chapter titles sit
	parms.effect = 2; // write-out scan, the chapter-title effect

	parms.r1 = r;
	parms.g1 = g;
	parms.b1 = b;
	parms.a1 = 255;

	// Colour 2 is the highlight on the character currently being written out.
	parms.r2 = 255;
	parms.g2 = 255;
	parms.b2 = 255;
	parms.a2 = 255;

	parms.fadeinTime = 0.05f; // per character while the effect is 2
	parms.fadeoutTime = 1.5f;
	parms.holdTime = 3.5f;
	parms.fxTime = 0.25f;

	// Its own channel so a later announcement never stomps this one mid-write.
	parms.channel = 1;

	UTIL_HudMessage(pPlayer, parms, pszText);
}

// ---------------------------------------------------------------------------
// Anonymous mode
//
// Role secrecy only goes so far while everyone is still playing under the name
// and colours they always play under: "the orange one is always the one who
// rushes" is a read on the Killer that the mode never meant to hand out. With
// ch_anonymous on, every round deals each player a colour off g_CHAnonColors
// and a name off the names file, and both are worn until the next deal.
//
// The disguise is a userinfo override, not an entity one, and that is what
// makes it reach everything: the studio renderer remaps a player model from
// the engine's copy of topcolor/bottomcolor, LeaveCorpse() builds a corpse's
// colormap out of the same two keys, and the scoreboard, chat and voice HUD
// all read the name key. Nothing else has to be taught about it.
//
// The player's model choice is deliberately left alone. It is a louder tell
// than colour ever was, but it is also the one piece of a player's appearance
// they picked on purpose, and taking it away costs more than it buys.
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::LoadAnonNames()
{
	g_numAnonNames = 0;

	int   fileSize = 0;
	byte* pMemFile = g_engfuncs.pfnLoadFileForMe(CH_ANON_NAME_FILE, &fileSize);

	if (pMemFile)
	{
		int pos = 0;

		while (pos < fileSize && g_numAnonNames < CH_MAX_ANON_NAMES)
		{
			// Take one line, minus its leading whitespace.
			while (pos < fileSize && (pMemFile[pos] == ' ' || pMemFile[pos] == '\t'))
				pos++;

			int len = 0;
			char szLine[CH_MAX_ANON_NAME];

			while (pos < fileSize && pMemFile[pos] != '\n' && pMemFile[pos] != '\r' && pMemFile[pos] != '\0')
			{
				if (len < CH_MAX_ANON_NAME - 1)
					szLine[len++] = static_cast<char>(pMemFile[pos]);

				pos++;
			}

			// Past the line terminator, whichever flavour it is. A stray NUL
			// counts as one: the read above stops on it, so leaving it here
			// would mean no loop advances and the parse never ends.
			while (pos < fileSize && (pMemFile[pos] == '\n' || pMemFile[pos] == '\r' || pMemFile[pos] == '\0'))
				pos++;

			// Trailing whitespace would show up in the name as-is.
			while (len > 0 && (szLine[len - 1] == ' ' || szLine[len - 1] == '\t'))
				len--;

			szLine[len] = '\0';

			if (len == 0 || (len >= 2 && szLine[0] == '/' && szLine[1] == '/'))
				continue;

			// A '%' in a player name is a format specifier by the time it
			// reaches a chat line; ClientUserInfoChanged() scrubs the ones
			// players type, so the file gets the same treatment.
			for (int i = 0; i < len; i++)
			{
				if (szLine[i] == '%')
					szLine[i] = ' ';
			}

			strcpy(g_szAnonNames[g_numAnonNames], szLine);
			g_numAnonNames++;
		}

		FREE_FILE(pMemFile);
	}

	if (g_numAnonNames == 0)
	{
		const int count = sizeof(g_szDefaultAnonNames) / sizeof(g_szDefaultAnonNames[0]);

		for (int i = 0; i < count && i < CH_MAX_ANON_NAMES; i++)
		{
			strncpy(g_szAnonNames[i], g_szDefaultAnonNames[i], CH_MAX_ANON_NAME - 1);
			g_szAnonNames[i][CH_MAX_ANON_NAME - 1] = '\0';
			g_numAnonNames++;
		}
	}
}

void CHalfLifeCrowbarHunt::StashRealIdentity(CBasePlayer* pPlayer, const char* pszInfoBuffer)
{
	const int index = ENTINDEX(pPlayer->edict());

	if (index < 1 || index > MAX_PLAYERS || m_realTopColor[index] >= 0)
		return;

	// Cast away const only because the engine's accessor is not const-correct;
	// InfoKeyValue does not write to the buffer.
	char* infobuffer = const_cast<char*>(pszInfoBuffer);

	strncpy(m_szRealName[index], g_engfuncs.pfnInfoKeyValue(infobuffer, "name"), CH_MAX_ANON_NAME - 1);
	m_szRealName[index][CH_MAX_ANON_NAME - 1] = '\0';

	m_realTopColor[index] = atoi(g_engfuncs.pfnInfoKeyValue(infobuffer, "topcolor"));
	m_realBottomColor[index] = atoi(g_engfuncs.pfnInfoKeyValue(infobuffer, "bottomcolor"));

	if (m_realTopColor[index] < 0)
		m_realTopColor[index] = 0;
	if (m_realBottomColor[index] < 0)
		m_realBottomColor[index] = 0;
}

void CHalfLifeCrowbarHunt::ApplyAnonIdentity(CBasePlayer* pPlayer)
{
	const int index = ENTINDEX(pPlayer->edict());

	if (index < 1 || index > MAX_PLAYERS || m_anonColor[index] < 0)
		return;

	char* infobuffer = g_engfuncs.pfnGetInfoKeyBuffer(pPlayer->edict());
	char  szValue[16];

	snprintf(szValue, sizeof(szValue), "%d", g_CHAnonColors[m_anonColor[index]].hue);
	g_engfuncs.pfnSetClientKeyValue(index, infobuffer, "topcolor", szValue);
	g_engfuncs.pfnSetClientKeyValue(index, infobuffer, "bottomcolor", szValue);

	g_engfuncs.pfnSetClientKeyValue(index, infobuffer, "name", m_szAnonName[index]);

	// The name key is what other clients see; pev->netname is what server-side
	// code reads, including our own DeathNotice() chat line. The engine keeps
	// the two in step on its own when a client sends userinfo, but not when the
	// DLL writes the key from underneath it.
	pPlayer->pev->netname = ALLOC_STRING(m_szAnonName[index]);
}

void CHalfLifeCrowbarHunt::DealAnonIdentity(CBasePlayer* pPlayer, int colorIndex, const char* pszName)
{
	const int index = ENTINDEX(pPlayer->edict());

	if (index < 1 || index > MAX_PLAYERS)
		return;

	StashRealIdentity(pPlayer, g_engfuncs.pfnGetInfoKeyBuffer(pPlayer->edict()));

	m_anonColor[index] = colorIndex;
	strncpy(m_szAnonName[index], pszName, CH_MAX_ANON_NAME - 1);
	m_szAnonName[index][CH_MAX_ANON_NAME - 1] = '\0';

	ApplyAnonIdentity(pPlayer);
}

void CHalfLifeCrowbarHunt::AssignAnonIdentities()
{
	if (ch_anonymous.value == 0)
	{
		if (m_bAnonActive)
			ClearAnonIdentities();

		return;
	}

	if (g_numAnonNames == 0)
		LoadAnonNames();

	// Dealt without replacement: two players in the same colour under the same
	// name is worse than no disguise at all, because it reads as a bug rather
	// than as two strangers. Past the end of either table the order wraps and
	// duplicates are the best that can be done.
	int colorOrder[CH_NUM_ANON_COLORS];
	int nameOrder[CH_MAX_ANON_NAMES];

	ShuffledOrder(colorOrder, CH_NUM_ANON_COLORS);
	ShuffledOrder(nameOrder, g_numAnonNames);

	int dealt = 0;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (!pPlayer)
			continue;

		DealAnonIdentity(pPlayer,
			colorOrder[dealt % CH_NUM_ANON_COLORS],
			g_szAnonNames[nameOrder[dealt % g_numAnonNames]]);

		dealt++;
	}

	m_bAnonActive = true;
	SendAnonColors(nullptr);
}

void CHalfLifeCrowbarHunt::ClearAnonIdentities()
{
	// Before the sends, so ShouldAnnounceNameChange() lets the engine talk
	// again and SendAnonColors() reports everyone as uncoloured.
	m_bAnonActive = false;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		m_anonColor[i] = -1;

		if (!pPlayer || m_realTopColor[i] < 0)
			continue;

		char* infobuffer = g_engfuncs.pfnGetInfoKeyBuffer(pPlayer->edict());
		char  szValue[16];

		snprintf(szValue, sizeof(szValue), "%d", m_realTopColor[i]);
		g_engfuncs.pfnSetClientKeyValue(i, infobuffer, "topcolor", szValue);
		snprintf(szValue, sizeof(szValue), "%d", m_realBottomColor[i]);
		g_engfuncs.pfnSetClientKeyValue(i, infobuffer, "bottomcolor", szValue);

		if (m_szRealName[i][0] != '\0')
		{
			g_engfuncs.pfnSetClientKeyValue(i, infobuffer, "name", m_szRealName[i]);
			pPlayer->pev->netname = ALLOC_STRING(m_szRealName[i]);
		}
	}

	SendAnonColors(nullptr);
}

void CHalfLifeCrowbarHunt::SendAnonColors(CBasePlayer* pTo) const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (!GetPlayerByIndex(i))
			continue;

		const int color = m_bAnonActive ? m_anonColor[i] : -1;

		if (pTo)
			MESSAGE_BEGIN(MSG_ONE, gmsgCHAnon, nullptr, pTo->pev);
		else
			MESSAGE_BEGIN(MSG_ALL, gmsgCHAnon, nullptr);

		WRITE_BYTE(i);
		WRITE_BYTE(color < 0 ? CH_ANON_NONE : color);

		// The scoreboard shows who is actually in the server, which the name
		// key can no longer say once it is carrying a disguise. This is the
		// only route the real name has to a client - and it is deliberately
		// the only place a client is allowed to use it. Empty while nobody is
		// disguised, which is the client's cue to go back to the name key.
		WRITE_STRING(color < 0 ? "" : m_szRealName[i]);

		MESSAGE_END();
	}
}

// A client that sends userinfo is sending their own name and colours, which is
// exactly what anonymous mode is covering up. Stamp the disguise back on.
void CHalfLifeCrowbarHunt::ClientUserInfoChanged(CBasePlayer* pPlayer, char* infobuffer)
{
	if (!pPlayer)
		return;

	// The first userinfo we ever see from a player is the only chance to read
	// their real name and colours off the wire, so take it whether or not
	// anonymous mode is on right now - the cvar can go on mid-map.
	StashRealIdentity(pPlayer, infobuffer);

	if (m_bAnonActive)
		ApplyAnonIdentity(pPlayer);
}

// ---------------------------------------------------------------------------
// Per-slot state
//
// Everything this mode knows about a player is held in an array indexed by
// client slot, and GoldSrc never frees a player edict or a client slot: a slot
// that comes free is handed straight to the next client to connect, carrying
// whatever the last occupant left in it.
//
// That has now caused three separate bugs, so the rule is: every array indexed
// by ENTINDEX() is cleared here and nowhere else, and a new one is not finished
// until it has a line in this function. Adding an array without touching this
// is the same class of mistake as adding a member without a m_SaveData entry -
// it works until the day something reuses the slot.
//
// The worst of the three was a stale role. A player joining mid-round is meant
// to read as Unassigned so PlayerSpawn() sits them out as an observer; if the
// slot's last occupant was a Survivor they spawned in live instead, and - being
// counted as a live Survivor by CheckRoundWinConditions() - also stopped the
// Killer from ever winning the round they were never part of. Reconnecting bots
// hit it every time, a kicked bot freeing a slot the next one takes back.
//
// Note this is *not* the same wipe AssignRoles() and ResetForNextRound() do
// between rounds: those clear roles only, and deliberately leave the anonymous
// identity alone so nobody's real name flashes up in the gap.
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::ResetPlayerSlot(int index)
{
	if (index < 0 || index > MAX_PLAYERS)
		return;

	m_playerRoles[index] = CHRole::Unassigned;
	m_flPunishEndTime[index] = 0.0f;
	m_flSendServerName[index] = 0.0f;
	m_iSentSpectator[index] = -1;
	m_flSpectatePromptExpires[index] = 0.0f;
	m_iLootCount[index] = 0;
	m_iLootRewards[index] = 0;

	// A fresh slot draws at full odds. This does mean a player who reconnects
	// sheds whatever repeat-suppression they had built up; the alternative is
	// keeping a table keyed by auth id, which bots (all "BOT") would share.
	m_flKillerWeight[index] = 1.0f;

	m_anonColor[index] = -1;
	m_szAnonName[index][0] = '\0';
	m_szRealName[index][0] = '\0';
	m_realTopColor[index] = -1;
	m_realBottomColor[index] = -1;
}

// Wiped at both ends of an occupancy rather than just on the way out, so a slot
// is clean even if the last tenant left by a route that never reached
// ClientDisconnected().
bool CHalfLifeCrowbarHunt::ClientConnected(edict_t* pEntity, const char* pszName, const char* pszAddress, char szRejectReason[128])
{
	if (pEntity)
		ResetPlayerSlot(ENTINDEX(pEntity));

	return CHalfLifeMultiplay::ClientConnected(pEntity, pszName, pszAddress, szRejectReason);
}

static void CH_DropRevolver(CBasePlayer* pPlayer);

void CHalfLifeCrowbarHunt::ClientDisconnected(edict_t* pClient)
{
	// First: the base class writes the disconnect log line, and it reads the
	// player's name to do it.
	CHalfLifeMultiplay::ClientDisconnected(pClient);

	if (!pClient)
		return;

	// A Hunter quitting mid-round would otherwise take the revolver out of the
	// round with them: ClientDisconnect() never unpacks the inventory, and the
	// drop in PlayerKilled() only fires for a death. Same box, same place, as
	// if they had died on the spot. The edict is still fully populated here -
	// only the disconnect log line has run before us.
	CBasePlayer* pPlayer = static_cast<CBasePlayer*>(CBaseEntity::Instance(pClient));

	if (pPlayer && pPlayer->IsAlive() && pPlayer->HasNamedPlayerItem("weapon_357"))
		CH_DropRevolver(pPlayer);

	ResetPlayerSlot(ENTINDEX(pClient));

	// Not part of the slot wipe above, because it is the engine's field rather
	// than one of ours: GetPlayerByIndex() decides whether a slot still has
	// somebody on the end of it by FL_CLIENT plus a non-empty name, and the
	// edict is never freed. Clearing the name makes that answer ours rather
	// than something we hope the engine did on its way out - and it is the
	// count built on it that decides whether a round can still go on.
	pClient->v.netname = 0;
}

// CHalfLifeMultiplay::RefreshSkillData() runs after the skill.cfg cvars have
// been read and overwrites the deathmatch weapons with its own hardcoded
// numbers, so setting sk_plr_crowbar / sk_plr_357_bullet has no effect in any
// multiplayer mode. Our values go on top of that pass, not underneath it.
void CHalfLifeCrowbarHunt::RefreshSkillData()
{
	CHalfLifeMultiplay::RefreshSkillData();

	gSkillData.plrDmgCrowbar = CH_CROWBAR_DAMAGE;
	gSkillData.plrDmg357 = CH_357_DAMAGE;
}

// The revolver holds one round (PYTHON_MAX_CLIP), so every shot costs the
// Hunter a two-second reload - that reload is the mode's real balance lever,
// not scarcity of ammo. Topping the reserve back up each frame keeps the
// reload mandatory while making the ammo behind it effectively endless.
void CHalfLifeCrowbarHunt::PlayerThink(CBasePlayer* pPlayer)
{
	CHalfLifeMultiplay::PlayerThink(pPlayer);

	if (!pPlayer)
		return;

	// Both run for everyone in every round state, so a player who stops being
	// the Killer - or stops sprinting, or serves out a penalty - drops back to
	// normal movement the same frame. Punishment first: it decides the speed.
	ServicePunishment(pPlayer);
	UpdatePlayerSpeed(pPlayer);
	ServiceServerNameSend(pPlayer);

	// Catches anyone alive in a live round without a role: a mid-round joiner,
	// or someone who connected during the countdown after AssignRoles() had
	// already run - plus anyone who asked to sit out, in any state. Idempotent:
	// PFLAG_OBSERVER means it is already done.
	EnforceObserverForSidelined(pPlayer);

	// After the above, so a player parked this frame is reported this frame.
	ServiceSpectatorState(pPlayer);

	if (!pPlayer->IsAlive() || m_roundState != CHRoundState::InProgress)
		return;

	// Only for whoever is actually carrying the revolver - a Survivor who picks
	// it up off the dead Hunter inherits the endless reserve with it.
	//
	// CBasePlayer::ammo_357 is not the ammo store, only a mirror of it that
	// TabulateAmmo() rewrites from m_rgAmmo[] several times a frame. The reload
	// that actually spends the reserve - CBasePlayerWeapon::ItemPostFrame() -
	// reads and writes m_rgAmmo[] directly, so topping up the mirror moved the
	// HUD number for one frame and left the real reserve draining to empty.
	if (pPlayer->HasNamedPlayerItem("weapon_357"))
	{
		const int iAmmoIndex = CBasePlayer::GetAmmoIndex("357");

		if (iAmmoIndex >= 0 && pPlayer->m_rgAmmo[iAmmoIndex] < _357_MAX_CARRY)
		{
			pPlayer->m_rgAmmo[iAmmoIndex] = _357_MAX_CARRY;

			// Put the mirror back in step in the same breath: CPython::Reload()
			// gates on ammo_357, and UpdateClientData() sends the HUD count from
			// m_rgAmmo[], so leaving the two disagreeing is what hid this bug.
			pPlayer->TabulateAmmo();
		}
	}
}

// The engine's movement code only ever clamps a player down: PM_CheckParamters()
// does maxspeed = min(pev->maxspeed, sv_maxspeed), and treats a pev->maxspeed of
// 0 as "no opinion". Raising one player above the rest therefore means lifting
// sv_maxspeed to the fastest speed the mode allows and holding every player
// under it by hand - which UpdatePlayerSpeed() does for all of them, every
// frame, so nobody is ever left running at the raised ceiling by accident.
void CHalfLifeCrowbarHunt::EnforceSpeedCeiling()
{
	const float flCeiling = V_max(ch_base_speed.value, ch_sprint_speed.value);

	if (CVAR_GET_FLOAT("sv_maxspeed") < flCeiling)
		CVAR_SET_FLOAT("sv_maxspeed", flCeiling);
}

// Sprinting is the Killer's alone: it is the pressure the mode runs on, and a
// Survivor who could match the Killer's speed would never need to hide. The
// client sends the key state in the otherwise unused IN_RUN button bit
// (cl_dll/input.cpp); this is where it is granted or ignored.
void CHalfLifeCrowbarHunt::UpdatePlayerSpeed(CBasePlayer* pPlayer) const
{
	// A penalty outranks everything else a role might grant, sprint included.
	if (IsPunished(pPlayer))
	{
		pPlayer->pev->maxspeed = CH_PUNISH_SPEED;
		return;
	}

	const bool bMaySprint =
		m_roundState == CHRoundState::InProgress &&
		GetPlayerRole(pPlayer) == CHRole::Killer &&
		pPlayer->IsAlive() &&
		(pPlayer->pev->button & IN_RUN) != 0;

	pPlayer->pev->maxspeed = bMaySprint ? ch_sprint_speed.value : ch_base_speed.value;
}

// ---------------------------------------------------------------------------
// Misfire punishment
//
// The revolver is the only real weapon in the mode and the Hunter is never told
// who the Killer is, so without a cost the strongest play is to shoot people
// until one of them turns out to have been the Killer. Guessing wrong puts the
// gun on the floor and leaves the shooter slow and unable to jump for
// ch_punish_time seconds - long enough for the Killer to reach them, and long
// enough that someone else may well have walked off with the revolver by the
// time they are allowed to hold one again.
// ---------------------------------------------------------------------------

// Hooked on damage rather than on a kill so a hit that only wounds still counts,
// and so the shooter is charged even when their shot is what ends the round.
bool CHalfLifeCrowbarHunt::FPlayerCanTakeDamage(CBasePlayer* pPlayer, CBaseEntity* pAttacker)
{
	if (m_roundState == CHRoundState::InProgress &&
		pPlayer && pAttacker && pAttacker != pPlayer && pAttacker->IsPlayer() &&
		GetPlayerRole(pPlayer) != CHRole::Killer)
	{
		CBasePlayer* pShooter = static_cast<CBasePlayer*>(pAttacker);

		// What the shooter is holding is what identifies the shot: this hook
		// sees every source of damage, and only the revolver is being policed.
		// The Killer's crowbar - thrown or swung - is the round working as
		// intended, and so is a Survivor pushing someone off a ledge. So is a
		// Killer shooting with the revolver loot bought them: the penalty is
		// for guessing wrong, and the Killer is never guessing.
		if (pShooter->m_pActiveItem && GetPlayerRole(pShooter) != CHRole::Killer &&
			FStrEq(STRING(pShooter->m_pActiveItem->pev->classname), "weapon_357"))
			PunishShooter(pShooter);
	}

	// The shot still lands. Missing the Killer costs the shooter, not the
	// person they hit - a bullet that stopped being lethal would make firing
	// blind into a crowd free.
	return CHalfLifeMultiplay::FPlayerCanTakeDamage(pPlayer, pAttacker);
}

void CHalfLifeCrowbarHunt::PunishShooter(CBasePlayer* pPlayer)
{
	if (ch_punish_time.value <= 0.0f)
		return;

	const int idx = ENTINDEX(pPlayer->edict());

	if (idx < 1 || idx > MAX_PLAYERS)
		return;

	m_flPunishEndTime[idx] = gpGlobals->time + ch_punish_time.value;

	SetPunishTint(pPlayer, true);

	// The gun is taken away a frame later, by ServicePunishment(). We are
	// called from inside CBasePlayer::TakeDamage(), which is itself inside the
	// revolver's own PrimaryAttack(): dropping it here would holster and repack
	// the weapon that is still part-way through firing.

	hudtextparms_t parms;
	memset(&parms, 0, sizeof(parms));

	parms.x = -1.0f;
	parms.y = 0.7f;
	parms.effect = 2;

	parms.r1 = 200;
	parms.g1 = 40;
	parms.b1 = 40;
	parms.a1 = 255;

	parms.r2 = 255;
	parms.g2 = 255;
	parms.b2 = 255;
	parms.a2 = 255;

	parms.fadeinTime = 0.05f;
	parms.fadeoutTime = 1.5f;
	parms.holdTime = 3.5f;
	parms.fxTime = 0.25f;

	// Channel 1 is AnnounceRole()'s; this is a different message with the same
	// weight, and the two can never be on screen at once anyway.
	parms.channel = 1;

	UTIL_HudMessage(pPlayer, parms,
		UTIL_VarArgs("THAT WASN'T THE KILLER\nYou drop the revolver. %d seconds crippled.",
			static_cast<int>(ch_punish_time.value)));
}

// The engine draws the fade over the world but under the HUD, so the penalty
// costs the shooter their sight of the map without also hiding the message
// telling them why.
void CHalfLifeCrowbarHunt::SetPunishTint(CBasePlayer* pPlayer, bool bOn)
{
	const int alpha = static_cast<int>(ch_punish_tint.value);

	if (alpha <= 0)
		return;

	// FFADE_STAYOUT holds at full alpha until the next ScreenFade message
	// arrives instead of timing out on its own. That is what lets one fade
	// cover a whole ch_punish_time: the duration field is 4.12 fixed point and
	// could not carry a penalty longer than about sixteen seconds.
	UTIL_ScreenFade(pPlayer, Vector(0, 0, 0),
		bOn ? CH_PUNISH_TINT_FADE_IN : CH_PUNISH_TINT_FADE_OUT, 0.0f,
		alpha, bOn ? (FFADE_OUT | FFADE_STAYOUT) : FFADE_IN);
}

bool CHalfLifeCrowbarHunt::IsPunished(CBasePlayer* pPlayer) const
{
	if (!pPlayer)
		return false;

	const int idx = ENTINDEX(pPlayer->edict());

	if (idx < 1 || idx > MAX_PLAYERS)
		return false;

	return m_flPunishEndTime[idx] != 0.0f && gpGlobals->time < m_flPunishEndTime[idx];
}

void CHalfLifeCrowbarHunt::ClearPunishments()
{
	for (int i = 0; i <= MAX_PLAYERS; i++)
	{
		if (m_flPunishEndTime[i] == 0.0f)
			continue;

		m_flPunishEndTime[i] = 0.0f;

		// A penalty cut short by the round ending still has to lift its own
		// tint - nothing else will, and the player would spend the next round
		// in the dark.
		if (CBaseEntity* pPlayer = UTIL_PlayerByIndex(i); pPlayer != nullptr)
			SetPunishTint(static_cast<CBasePlayer*>(pPlayer), false);
	}
}

// kRenderFxGlowShell is drawn by the studio renderer as a second pass over the
// model with chrome forced on (see CStudioModelRenderer::StudioRenderModel), so
// it only works on studio models - which both of these pickups are - and it is
// occluded by walls like the model itself. renderamt is the shell thickness,
// not an alpha.
void CH_SetWeaponGlow(CBaseEntity* pEntity, CHWeaponGlow weapon)
{
	if (!pEntity)
		return;

	const int thickness = static_cast<int>(ch_glow_shell.value);

	if (thickness <= 0)
		return;

	pEntity->pev->renderfx = kRenderFxGlowShell;
	pEntity->pev->renderamt = thickness;

	switch (weapon)
	{
	case CHWeaponGlow::Crowbar:
		pEntity->pev->rendercolor = Vector(200, 20, 20);
		break;
	case CHWeaponGlow::Revolver:
		pEntity->pev->rendercolor = Vector(20, 60, 220);
		break;
	case CHWeaponGlow::Loot:
		pEntity->pev->rendercolor = Vector(20, 200, 40);
		break;
	}
}

// Drop the Hunter's revolver so it can be inherited.
//
// DropPlayerItem() always packs into a CWeaponBox, whose world model is the
// generic w_weaponbox.mdl duffel bag. That reads as "someone's whole kit" -
// misleading in a mode where the revolver is the only gun in play - so the box
// is reskinned to the revolver's own world model right after it is created.
// The box entity is kept (rather than spawning a bare weapon_357) because it
// carries the ammo across with the gun and its Touch() already honours
// CanHavePlayerItem(), which is what gates non-Hunters from picking it up.
//
// DropPlayerItem() returns void, so the new box is found by walking the
// weaponboxes owned by this player - all of which are ours, since
// DeadPlayerWeapons() drops nothing on its own.
static void CH_DropRevolver(CBasePlayer* pPlayer)
{
	// DropPlayerItem() takes a mutable string.
	char szRevolver[] = "weapon_357";
	pPlayer->DropPlayerItem(szRevolver);

	CBaseEntity* pBox = nullptr;

	while ((pBox = UTIL_FindEntityByClassname(pBox, "weaponbox")) != nullptr)
	{
		if (pBox->pev->owner != pPlayer->edict())
			continue;

		CWeaponBox* pWeaponBox = static_cast<CWeaponBox*>(pBox);
		bool bHasRevolver = false;

		for (int i = 0; i < MAX_ITEM_TYPES; i++)
		{
			for (CBasePlayerItem* pItem = pWeaponBox->m_rgpPlayerItems[i]; pItem != nullptr; pItem = pItem->m_pNext)
			{
				if (FStrEq(STRING(pItem->pev->classname), "weapon_357"))
					bHasRevolver = true;
			}
		}

		if (!bHasRevolver)
			continue;

		// Precached by CPython::Precache(), which runs for every round since
		// the Hunter is always given one.
		SET_MODEL(ENT(pWeaponBox->pev), "models/w_357.mdl");

		// SET_MODEL adopts the model's own bounds; the box relies on the empty
		// size CWeaponBox::Spawn() set (SetObjectCollisionBox() expands it for
		// touch), so put it back.
		UTIL_SetSize(pWeaponBox->pev, g_vecZero, g_vecZero);

		CH_SetWeaponGlow(pWeaponBox, CHWeaponGlow::Revolver);
	}
}

// Run every frame for every player, from PlayerThink().
void CHalfLifeCrowbarHunt::ServicePunishment(CBasePlayer* pPlayer)
{
	const int idx = ENTINDEX(pPlayer->edict());

	if (idx < 1 || idx > MAX_PLAYERS)
		return;

	if (m_flPunishEndTime[idx] != 0.0f && gpGlobals->time >= m_flPunishEndTime[idx])
	{
		m_flPunishEndTime[idx] = 0.0f;
		SetPunishTint(pPlayer, false);
		//ClientPrint(pPlayer->pev, HUD_PRINTCENTER,
		//	"Penalty served.\nYou can move - and carry a revolver - again.\n");
	}

	const bool bPunished = m_flPunishEndTime[idx] != 0.0f;

	// Deferred out of PunishShooter() - see the note there. Checked every frame
	// rather than once so it also catches a revolver that arrives mid-penalty;
	// CanHavePlayerItem() should stop that, but losing the gun is the point of
	// the penalty and is not worth trusting to a single gate.
	if (bPunished && pPlayer->IsAlive() && pPlayer->HasNamedPlayerItem("weapon_357"))
	{
		CH_DropRevolver(pPlayer);
	}

	UpdatePlayerJump(pPlayer, bPunished);
}

// Speed can be clamped server-side because PM_CheckParamters() reads
// pev->maxspeed, but jump height is a constant inside PM_Jump(), which runs on
// the client too. Clamping it only on the server would rubber-band every jump,
// so the height goes out in the client's physinfo string - the same channel the
// longjump module uses - and PM_Jump() reads it on both sides.
//
// Only written when it changes: physinfo is networked, and this runs every frame.
void CHalfLifeCrowbarHunt::UpdatePlayerJump(CBasePlayer* pPlayer, bool bPunished)
{
	const char* pszWanted = bPunished ? CH_PUNISH_JUMP_PERCENT : CH_NORMAL_JUMP_PERCENT;
	const char* pszCurrent = g_engfuncs.pfnGetPhysicsKeyValue(pPlayer->edict(), "chjs");

	if (!pszCurrent || !FStrEq(pszCurrent, pszWanted))
		g_engfuncs.pfnSetPhysicsKeyValue(pPlayer->edict(), "chjs", pszWanted);
}

// A role's weapon is part of its identity, so weapons can't change hands: a
// Survivor holding the crowbar would read as the Killer, and the Killer with
// the revolver ends the round on the spot. This covers every pickup route -
// map pickups, GiveNamedItem() (which touches the item onto the player), and
// the boxes dropped by dead players.
bool CHalfLifeCrowbarHunt::RoleCanCarryWeapon(CHRole role, const char* pszWeaponName)
{
	if (FStrEq(pszWeaponName, "weapon_crowbar"))
		return role == CHRole::Killer;

	// The Hunter's revolver is inheritable: if the Hunter dies, a Survivor who
	// reaches the body can take up the fight. The Killer never can.
	if (FStrEq(pszWeaponName, "weapon_357"))
		return role != CHRole::Killer;

	return true;
}

bool CHalfLifeCrowbarHunt::CanHavePlayerItem(CBasePlayer* pPlayer, CBasePlayerItem* pItem)
{
	// Weapons only exist while a round is live. Outside that every route in is
	// closed - the crowbar and glock CHalfLifeMultiplay::PlayerSpawn() hands out
	// on every respawn, leftover weaponboxes, map pickups - so nobody can still
	// be holding something when StartRound() deals out the real loadouts.
	if (m_roundState != CHRoundState::InProgress)
		return false;

	if (pPlayer && pItem && !RoleCanCarryWeapon(GetPlayerRole(pPlayer), STRING(pItem->pev->classname)) &&
		!(m_bGrantingKillerGun && FStrEq(STRING(pItem->pev->classname), "weapon_357")))
		return false;

	// While the penalty is running the shooter can't rearm - not with the gun
	// they dropped, and not with one off anybody else's body. Once it expires
	// they can pick a revolver up again like any other Survivor.
	if (pPlayer && pItem && IsPunished(pPlayer) &&
		FStrEq(STRING(pItem->pev->classname), "weapon_357"))
		return false;

	return CHalfLifeMultiplay::CanHavePlayerItem(pPlayer, pItem);
}

// Ammo needs the same gate as the weapon it feeds. CWeaponBox::Touch() deals
// its ammo out before it asks the gamerules about its weapons, so the Killer
// walking over the dead Hunter's box would take the 357 rounds with him even
// though the revolver itself stays in the box - leaving the Survivor who
// inherits the gun with nothing behind the round already in the chamber.
bool CHalfLifeCrowbarHunt::CanHaveAmmo(CBasePlayer* pPlayer, const char* pszAmmoName, int iMaxCarry)
{
	// Same reasoning as CanHavePlayerItem(): between rounds nothing is
	// collectable, so a leftover box can't be looted during the reset.
	if (m_roundState != CHRoundState::InProgress)
		return false;

	// Asked through the same helper as the weapon so the two can never
	// disagree about who the revolver belongs to.
	if (pPlayer && pszAmmoName && FStrEq(pszAmmoName, "357") &&
		!RoleCanCarryWeapon(GetPlayerRole(pPlayer), "weapon_357"))
		return false;

	// And the same for a punished shooter, or they would strip the rounds out
	// of the box they just dropped and leave the next Hunter with one shot.
	if (pPlayer && pszAmmoName && FStrEq(pszAmmoName, "357") && IsPunished(pPlayer))
		return false;

	return CHalfLifeMultiplay::CanHaveAmmo(pPlayer, pszAmmoName, iMaxCarry);
}

bool CHalfLifeCrowbarHunt::CanPlayerHearPlayer(CBasePlayer* pListener, CBasePlayer* pTalker)
{
	if (0 == ch_proxvoice.value)
		return true;

	// Only a live round is worth keeping quiet. Between rounds everyone is
	// standing around waiting, so let the lobby talk.
	if (m_roundState != CHRoundState::InProgress)
		return true;

	if (pListener == pTalker)
		return true;

	// StartObserver() leaves the dead on DEAD_RESPAWNABLE, so IsAlive() is what
	// separates players still in the round from those watching it.
	if (!pTalker->IsAlive())
	{
		// The dead talk freely among themselves, but nothing they say reaches
		// anyone still playing - otherwise dying is how you name the Killer.
		return !pListener->IsAlive();
	}

	// An observer's origin is wherever they died, not what they are watching,
	// so distance would be meaningless for them. Let them hear the whole map.
	if (!pListener->IsAlive())
		return true;

	return (pTalker->pev->origin - pListener->pev->origin).Length() <= CH_PROXVOICE_RADIUS;
}

void CHalfLifeCrowbarHunt::GiveRoleLoadout(CBasePlayer* pPlayer, CHRole role)
{
	if (!pPlayer)
		return;

	pPlayer->RemoveAllItems(false); // strip default HEV/suit weapons first

	// Everyone carries empty hands, whatever their role. It is worth nothing in
	// a fight - it is how you put your weapon away, so the Killer can walk
	// around looking as harmless as the Survivors. Given first so that the real
	// weapon below, being heavier, is what the player actually spawns holding.
	pPlayer->GiveNamedItem("weapon_hands");

	switch (role)
	{
	case CHRole::Killer:
		pPlayer->GiveNamedItem("weapon_crowbar");
		break;

	case CHRole::Hunter:
		pPlayer->GiveNamedItem("weapon_357");
		pPlayer->GiveAmmo(_357_MAX_CARRY, "357", _357_MAX_CARRY);
		break;

	case CHRole::Survivor:
	default:
		// Unarmed on purpose.
		break;
	}
}

bool CHalfLifeCrowbarHunt::IsSittingOut(int index) const
{
	if (index < 1 || index > MAX_PLAYERS)
		return false;

	return m_playerRoles[index] == CHRole::Spectator;
}

// ---------------------------------------------------------------------------
// Voluntary spectating
//
// The stock "spectate" command is a one-way door: it drops the player into
// observer mode and leaves no record that they chose it. That is enough for
// deathmatch, where nothing ever respawns a player who does not ask to be, but
// this mode puts everybody back on a spawn point at the top of each round - so
// a spectator was being dragged back into play one round later.
//
// CHRole::Spectator is that missing record, and it is what every head count,
// role deal and respawn sweep checks. Typing "spectate" again is the way back
// in: there is no other un-spectate path in the SDK to hook, and the engine's
// own spectator UI only ever sends this one command.
// ---------------------------------------------------------------------------
bool CHalfLifeCrowbarHunt::HandleSpectateCommand(CBasePlayer* pPlayer)
{
	if (!pPlayer)
		return false;

	const int index = ENTINDEX(pPlayer->edict());

	if (index < 1 || index > MAX_PLAYERS)
		return false;

	if (IsSittingOut(index))
	{
		// Unassigned, not Survivor: a deal is the only thing that hands out a
		// part in a round, so rejoining asks for the next one rather than
		// inserting them into the round already running.
		m_playerRoles[index] = CHRole::Unassigned;

		// Between rounds there is a live body to go back to, so take it now -
		// FPlayerCanRespawn() is already the answer to "is there a round in the
		// way". Mid-round they keep the camera they are in, and the next deal
		// picks them up.
		if (FPlayerCanRespawn(pPlayer))
			ForceRespawn(pPlayer);

		ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "Rejoining.\nYou're in for the next round.\n");

		return true;
	}

	// Walking out of a live round is a loss to it, and "spectate" is one
	// keypress on the engine's own spectator UI - so ask first. The yes comes
	// back as "menuselect 1" (see ClientCommand()); no, or silence, is a no.
	// Between rounds there is nothing to lose, so no prompt. Typing spectate a
	// second time while the prompt is still up counts as the yes.
	if (m_roundState == CHRoundState::InProgress && pPlayer->IsAlive() && gpGlobals->time > m_flSpectatePromptExpires[index])
	{
		m_flSpectatePromptExpires[index] = gpGlobals->time + CH_SPECTATE_PROMPT_TIME;

		MESSAGE_BEGIN(MSG_ONE, gmsgShowMenu, NULL, pPlayer->edict());
		WRITE_SHORT((1 << 0) | (1 << 1)); // keys 1 and 2
		WRITE_CHAR(CH_SPECTATE_PROMPT_TIME);
		WRITE_BYTE(0); // no more text to follow
		WRITE_STRING("Sit out the rest of this round?\nYou will stay out until you type spectate again.\n\n1. Yes, spectate\n2. No, keep playing");
		MESSAGE_END();

		return true;
	}

	BecomeSpectator(pPlayer);

	return true;
}

// The half of "spectate" that actually leaves: reached straight away between
// rounds, and through the prompt's yes during one.
void CHalfLifeCrowbarHunt::BecomeSpectator(CBasePlayer* pPlayer)
{
	const int index = ENTINDEX(pPlayer->edict());

	m_flSpectatePromptExpires[index] = 0.0f;
	m_playerRoles[index] = CHRole::Spectator;
	SendRoleHud(pPlayer);

	// Leaving a live round mid-way is the same loss to it as dying: the role is
	// gone from CountAlivePlayersWithRole(), so CheckRoundWinConditions() settles
	// the round on the next frame - including the case of the Killer walking out.
	if ((pPlayer->m_afPhysicsFlags & PFLAG_OBSERVER) == 0)
	{
		// The tint would otherwise follow a punished player into the spectator
		// camera, the same way it would a dead one. See MoveDeadPlayersToObserver().
		if (IsPunished(pPlayer))
			SetPunishTint(pPlayer, false);

		// StartObserver() holsters but keeps the inventory, so a Hunter sitting
		// out would carry the revolver into the camera and out of the round.
		// The dead already dropped theirs in PlayerKilled(); this is the
		// walking-out equivalent.
		if (pPlayer->IsAlive() && pPlayer->HasNamedPlayerItem("weapon_357"))
			CH_DropRevolver(pPlayer);

		pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->v_angle);
	}

	ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "Spectating.\nType spectate again to rejoin.\n");
}

bool CHalfLifeCrowbarHunt::ClientCommand(CBasePlayer* pPlayer, const char* pcmd)
{
	if (CHalfLifeMultiplay::ClientCommand(pPlayer, pcmd))
		return true;

	if (!pPlayer)
		return false;

	// "setpos x y z" and "setang pitch yaw [roll]" - teleport, and turn to
	// face a direction, for checking loot spots against what "status" printed.
	// Cheats, so both need sv_cheats.
	const bool setpos = FStrEq(pcmd, "setpos");
	const bool setang = FStrEq(pcmd, "setang");

	if (setpos || setang)
	{
		if (CVAR_GET_FLOAT("sv_cheats") == 0)
		{
			ClientPrint(pPlayer->pev, HUD_PRINTCONSOLE, UTIL_VarArgs("%s: sv_cheats is off\n", pcmd));
			return true;
		}

		if (setpos)
		{
			if (CMD_ARGC() < 4)
			{
				ClientPrint(pPlayer->pev, HUD_PRINTCONSOLE, "usage: setpos <x> <y> <z>\n");
				return true;
			}

			const Vector vecPos(atof(CMD_ARGV(1)), atof(CMD_ARGV(2)), atof(CMD_ARGV(3)));

			pPlayer->pev->velocity = g_vecZero;
			UTIL_SetOrigin(pPlayer->pev, vecPos);
			return true;
		}

		if (CMD_ARGC() < 3)
		{
			ClientPrint(pPlayer->pev, HUD_PRINTCONSOLE, "usage: setang <pitch> <yaw> [roll]\n");
			return true;
		}

		const Vector vecAngles(atof(CMD_ARGV(1)), atof(CMD_ARGV(2)), CMD_ARGC() > 3 ? atof(CMD_ARGV(3)) : 0.0);

		// v_angle is the view; angles is the body (pitch stored a third and
		// negated, the way the movement code keeps it); fixangle makes the
		// engine push the new view to the client, which otherwise owns it.
		pPlayer->pev->v_angle = vecAngles;
		pPlayer->pev->angles = vecAngles;
		pPlayer->pev->angles.x = -vecAngles.x / 3.0f;
		pPlayer->pev->fixangle = 1;
		return true;
	}

	if (!FStrEq(pcmd, "menuselect"))
		return false;

	const int index = ENTINDEX(pPlayer->edict());

	if (index < 1 || index > MAX_PLAYERS)
		return true;

	// Only the spectate prompt puts a menu up in this mode, and only its answer
	// is listened for. Anything past the prompt's own timeout is an answer to
	// something else.
	const bool prompting = m_flSpectatePromptExpires[index] != 0.0f && gpGlobals->time <= m_flSpectatePromptExpires[index];
	m_flSpectatePromptExpires[index] = 0.0f;

	if (!prompting || CMD_ARGC() < 2)
		return true;

	// A player killed while the prompt was up is already on their way to the
	// observer camera; let that path finish rather than start it twice.
	if (atoi(CMD_ARGV(1)) == 1 && !IsSittingOut(index) && pPlayer->IsAlive())
		BecomeSpectator(pPlayer);

	return true;
}

void CHalfLifeCrowbarHunt::SetPlayerRole(CBasePlayer* pPlayer, CHRole role)
{
	if (!pPlayer)
		return;

	int idx = ENTINDEX(pPlayer->edict());
	if (idx < 1 || idx > MAX_PLAYERS)
		return;

	m_playerRoles[idx] = role;
}

CHRole CHalfLifeCrowbarHunt::GetPlayerRole(CBasePlayer* pPlayer) const
{
	if (!pPlayer)
		return CHRole::Unassigned;

	int idx = ENTINDEX(pPlayer->edict());
	if (idx < 1 || idx > MAX_PLAYERS)
		return CHRole::Unassigned;

	return m_playerRoles[idx];
}

// ---------------------------------------------------------------------------
// Player iteration
// ---------------------------------------------------------------------------
CBasePlayer* CHalfLifeCrowbarHunt::GetPlayerByIndex(int index)
{
	// UTIL_PlayerByIndex() only rules out slots the engine never allocated: it
	// checks edict->free, and a player edict is *not* freed when that client
	// disconnects - it stays allocated, still a CBasePlayer, for the rest of
	// the map. So every count built on it keeps counting everyone who has ever
	// been on the server, which is why a lone remaining player never took the
	// round below CH_MIN_PLAYERS. FL_CLIENT plus a non-empty netname is what
	// actually says someone is on the other end of the slot.
	CBasePlayer* pPlayer = static_cast<CBasePlayer*>(UTIL_PlayerByIndex(index));

	if (!pPlayer || !pPlayer->IsNetClient())
		return nullptr;

	const char* pszName = STRING(pPlayer->pev->netname);

	if (!pszName || '\0' == pszName[0])
		return nullptr;

	return pPlayer;
}

// Counts bodies available to a round, which is not the same as clients on the
// server: somebody who has asked to sit out cannot fill a slot in one. This is
// what decides whether a round can start or has to be aborted, so a server that
// empties out into spectators correctly drops back to waiting.
int CHalfLifeCrowbarHunt::CountConnectedPlayers() const
{
	int count = 0;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (GetPlayerByIndex(i) && !IsSittingOut(i))
			count++;
	}

	return count;
}

int CHalfLifeCrowbarHunt::CountAlivePlayersWithRole(CHRole role) const
{
	int count = 0;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (!pPlayer || !pPlayer->IsAlive())
			continue;

		if (m_playerRoles[i] == role)
			count++;
	}

	return count;
}

// ---------------------------------------------------------------------------
// Killer draw weighting
//
// A flat draw hands the same player the crowbar twice in a row often enough to
// feel broken - on a five-player server that is one round in five - and being
// the Killer is the round that everyone else is playing against, so a repeat
// costs the room a round as well as the player. Every slot therefore carries a
// weight and the draw is proportional to it: being drawn multiplies the
// weight by ch_killer_decay, and every round spent not being drawn adds
// ch_killer_recover back, up to the 1.0 everyone starts at.
//
// Weights only move on rounds a player was actually in the draw for. Sitting
// out as an observer neither improves nor spends anyone's odds.
// ---------------------------------------------------------------------------
bool CHalfLifeCrowbarHunt::IsKillerCandidate(int index) const
{
	CBasePlayer* pPlayer = GetPlayerByIndex(index);

	return pPlayer && pPlayer->IsAlive();
}

float CHalfLifeCrowbarHunt::GetKillerWeight(int index) const
{
	if (index < 1 || index > MAX_PLAYERS)
		return 0.0f;

	return m_flKillerWeight[index];
}

float CHalfLifeCrowbarHunt::TotalKillerWeight() const
{
	float flTotal = 0.0f;

	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; i++)
	{
		if (IsKillerCandidate(i))
			flTotal += m_flKillerWeight[i];
	}

	return flTotal;
}

float CHalfLifeCrowbarHunt::GetKillerChancePercent(int index) const
{
	if (index < 1 || index > MAX_PLAYERS || !IsKillerCandidate(index))
		return 0.0f;

	const float flTotal = TotalKillerWeight();

	if (flTotal <= 0.0f)
		return 0.0f;

	return (m_flKillerWeight[index] / flTotal) * 100.0f;
}

CBasePlayer* CHalfLifeCrowbarHunt::PickWeightedKiller() const
{
	const float flTotal = TotalKillerWeight();

	// Nobody eligible, or every weight has somehow been driven to zero - fall
	// back to the flat draw rather than leaving the round without a Killer.
	if (flTotal <= 0.0f)
		return PickRandomAlivePlayer();

	// Walk the candidates subtracting their weight from a point chosen
	// uniformly along the total, and stop at whoever the point lands inside.
	float flRoll = RANDOM_FLOAT(0.0f, flTotal);

	CBasePlayer* pLast = nullptr;

	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; i++)
	{
		if (!IsKillerCandidate(i))
			continue;

		pLast = GetPlayerByIndex(i);
		flRoll -= m_flKillerWeight[i];

		if (flRoll <= 0.0f)
			return pLast;
	}

	// RANDOM_FLOAT is inclusive at the top, so a roll of exactly flTotal walks
	// off the end of the list. That is the last candidate.
	return pLast;
}

void CHalfLifeCrowbarHunt::AgeKillerWeights(CBasePlayer* pKiller)
{
	const float flDecay = ch_killer_decay.value;
	const float flRecover = ch_killer_recover.value;

	// A decay of 1 (or higher) means "don't ration the role at all". Leave the
	// weights alone entirely so flipping the cvar back on resumes from where
	// the server left off rather than from a table of drifted numbers.
	if (flDecay >= 1.0f)
		return;

	const float flMin = V_max(0.0f, ch_killer_min_weight.value);
	const int   iKillerIndex = pKiller ? ENTINDEX(pKiller->edict()) : 0;

	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; i++)
	{
		if (!IsKillerCandidate(i))
			continue;

		if (i == iKillerIndex)
			m_flKillerWeight[i] = V_max(flMin, m_flKillerWeight[i] * V_max(0.0f, flDecay));
		else
			m_flKillerWeight[i] = V_min(1.0f, m_flKillerWeight[i] + V_max(0.0f, flRecover));
	}
}

CBasePlayer* CHalfLifeCrowbarHunt::PickRandomAlivePlayer(CHRole excludeRole) const
{
	CBasePlayer* candidates[MAX_PLAYERS];
	int          numCandidates = 0;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (!pPlayer || !pPlayer->IsAlive())
			continue;

		if (excludeRole != CHRole::Unassigned && m_playerRoles[i] == excludeRole)
			continue;

		candidates[numCandidates++] = pPlayer;
	}

	if (numCandidates == 0)
		return nullptr;

	return candidates[RANDOM_LONG(0, numCandidates - 1)];
}

// ---------------------------------------------------------------------------
// Spawning / respawning
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::PlayerSpawn(CBasePlayer* pPlayer)
{
	if (!pPlayer)
		return;

	const int index = ENTINDEX(pPlayer->edict());

	m_flSendServerName[index] = gpGlobals->time + CH_SERVERNAME_SEND_DELAY;

	// Somebody who connected after this round's deal still needs covering: they
	// would otherwise sit on the scoreboard under their own name for the rest
	// of the round. Picked at random rather than dealt, so it can collide with
	// a colour already in play - they are spectating until the next deal, where
	// they are included properly.
	if (m_bAnonActive && m_anonColor[index] < 0 && g_numAnonNames > 0)
	{
		DealAnonIdentity(pPlayer,
			RANDOM_LONG(0, CH_NUM_ANON_COLORS - 1),
			g_szAnonNames[RANDOM_LONG(0, g_numAnonNames - 1)]);

		SendAnonColors(nullptr);
	}

	// Deliberately not CHalfLifeMultiplay::PlayerSpawn(). That hands every
	// spawning player a crowbar and a glock, and GiveNamedItem() delivers them
	// by spawning a real world entity at the player's feet and touching them
	// with it. When CanHavePlayerItem() refuses - which it does for every
	// spawn outside a live round - the entity is not removed, it just stays
	// lying on the spawn point as a free pickup. Loadouts come from
	// GiveRoleLoadout() instead, so the default equip is never wanted here.
	pPlayer->SetHasSuit(true);

	// StartObserver() hides the health/armour and weapon/ammo HUD panels and
	// CBasePlayer::Spawn() does not put m_iHideHUD back, so a player coming out
	// of observer mode would keep the spectator HUD flags for the rest of the
	// map. The flashlight bit is the only one PlayerPreThink() recomputes every
	// frame, which is why that icon alone kept working. Clear them here, before
	// the Unassigned branch below puts the player straight back into observer.
	pPlayer->m_iHideHUD = 0;

	CHRole role = GetPlayerRole(pPlayer);

	// Sitting out by choice. Nothing in the round loop spawns these players, so
	// reaching here means something else did - the engine putting a reconnecting
	// client in, say. Strip them and let PlayerThink() put the camera back.
	if (role == CHRole::Spectator)
	{
		pPlayer->RemoveAllItems(false);
		return;
	}

	if (m_roundState != CHRoundState::InProgress)
	{
		pPlayer->RemoveAllItems(false); // no weapons while waiting/pre-round
		return;
	}

	if (role == CHRole::Unassigned)
	{
		// Connected after roles were handed out - sit this round out rather
		// than joining as a free extra Survivor. The observer transition is
		// deliberately not done here: for a joining client this runs inside
		// CBasePlayer::Spawn(), and ClientPutInServer() zeroes iuser1/iuser2
		// immediately afterwards, cancelling the spectator view and leaving
		// the joiner playing the round as an invisible noclipping player.
		// PlayerThink() applies it a frame later, once nothing undoes it.
		pPlayer->RemoveAllItems(false);
		ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "Round in progress.\nYou'll join the next one.\n");
		return;
	}

	GiveRoleLoadout(pPlayer, role);
}

bool CHalfLifeCrowbarHunt::FPlayerCanRespawn(CBasePlayer* pPlayer)
{
	// No mid-round respawning - once you're dead, you spectate until the
	// round has been reset. RoundEnd counts as part of the round: the player
	// whose death ended it would otherwise be able to respawn live during the
	// result screen. Only WaitingForPlayers/PreRound let a player back in.
	return m_roundState == CHRoundState::WaitingForPlayers
		|| m_roundState == CHRoundState::PreRound;
}

// Put a player back on a spawn point, clearing observer mode if they were in
// it. Mirrors what ClientPutInServer() does for a joining player.
void CHalfLifeCrowbarHunt::ForceRespawn(CBasePlayer* pPlayer) const
{
	if (!pPlayer)
		return;

	pPlayer->Spawn(); // resets m_afPhysicsFlags, so PFLAG_OBSERVER goes with it

	pPlayer->pev->effects |= EF_NOINTERP;
	pPlayer->pev->iuser1 = 0; // disable any spec modes
	pPlayer->pev->iuser2 = 0;

	// Spawn() sets m_fInitHUD, so the next UpdateClientData() sends gmsgResetHUD
	// and the client wipes its HUD state - health, the weapon list, ammo. What it
	// does *not* do is invalidate the server's cache of what the client already
	// knows: m_iClientHealth still reads 100 and m_fKnownItem is still true, so
	// neither the health value nor the weapon list is ever sent again and those
	// HUD elements stay dead for the rest of the map. (Only the battery survived,
	// because Spawn() does reset m_iClientBattery.) Most visible on the second
	// player to join a WaitingForPlayers server: their connect Spawn() is followed
	// immediately by this one when the join takes the server to CH_MIN_PLAYERS.
	// ForceClientDllUpdate() is the SDK's own "resend everything" path.
	pPlayer->ForceClientDllUpdate();
}

// Belt-and-braces wipe. PlayerSpawn() already strips anyone it respawns, but
// this does not depend on a player having gone through a spawn at all, so it
// also catches observers and anyone the respawn loop skipped.
void CHalfLifeCrowbarHunt::StripAllPlayers() const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (pPlayer)
			pPlayer->RemoveAllItems(false);
	}
}

// Both sweeps leave anyone sitting out where they are. This is the whole reason
// the Spectator role exists: StartObserver() leaves a player reading as not
// alive, so without the check a voluntary spectator looks exactly like a player
// who died last round and is owed a respawn.
void CHalfLifeCrowbarHunt::ForceRespawnAllPlayers() const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (!IsSittingOut(i))
			ForceRespawn(GetPlayerByIndex(i));
	}
}

void CHalfLifeCrowbarHunt::ForceRespawnDeadPlayers() const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (pPlayer && !pPlayer->IsAlive() && !IsSittingOut(i))
			ForceRespawn(pPlayer);
	}
}

// Drop players who have finished their death animation into observer mode, so
// they can watch the rest of the round instead of staring at their own corpse.
void CHalfLifeCrowbarHunt::MoveDeadPlayersToObserver() const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (!pPlayer || pPlayer->IsAlive())
			continue;

		if ((pPlayer->m_afPhysicsFlags & PFLAG_OBSERVER) != 0)
			continue; // already spectating

		// PlayerDeathThink() advances deadflag to DEAD_DEAD once the death
		// animation has played out; going early would cut it off mid-fall.
		if (pPlayer->pev->deadflag != DEAD_DEAD)
			continue;

		// The rest of a penalty means nothing to a dead player, but the tint
		// would follow them into the spectator camera and leave them watching
		// the round through it. The timer keeps running: only the view is
		// given back.
		if (IsPunished(pPlayer))
			SetPunishTint(pPlayer, false);

		LeaveCorpse(pPlayer);
		pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->v_angle);
	}
}

// The other half of PlayerSpawn()'s Unassigned and Spectator branches, run from
// PlayerThink() so ClientPutInServer() can no longer undo it.
//
// Two kinds of player end up here. A roleless one alive in a live round missed
// role assignment, and watches until ResetForNextRound() takes everybody back
// out - so that case is gated to InProgress. A voluntary spectator asked to sit
// out and is checked in every state, because there is no round boundary that
// ends it: only asking to come back does.
void CHalfLifeCrowbarHunt::EnforceObserverForSidelined(CBasePlayer* pPlayer) const
{
	const CHRole role = GetPlayerRole(pPlayer);

	if (role == CHRole::Unassigned)
	{
		if (m_roundState != CHRoundState::InProgress)
			return;
	}
	else if (role != CHRole::Spectator)
	{
		return;
	}

	if (!pPlayer->IsAlive())
		return;

	if ((pPlayer->m_afPhysicsFlags & PFLAG_OBSERVER) != 0)
		return; // already spectating

	pPlayer->RemoveAllItems(false);
	pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->v_angle);
}

void CHalfLifeCrowbarHunt::ServiceServerNameSend(CBasePlayer* pPlayer)
{
	const int index = ENTINDEX(pPlayer->edict());

	if (m_flSendServerName[index] == 0.0f || gpGlobals->time < m_flSendServerName[index])
		return;

	m_flSendServerName[index] = 0.0f;

	MESSAGE_BEGIN(MSG_ONE, gmsgServerName, NULL, pPlayer->edict());
	WRITE_STRING(CVAR_GET_STRING("hostname"));
	MESSAGE_END();

	// Anonymous colours ride the same delay for the same reason: a client that
	// has not hooked its user messages yet drops them. A joiner who misses this
	// send still picks everything up at the next round's deal.
	SendAnonColors(pPlayer);

	// So does the game mode, and the sit-out state of everyone already here -
	// ServiceSpectatorState() only broadcasts changes, and this client missed
	// the ones that happened before it arrived.
	UpdateGameMode(pPlayer);

	// And the round clock, if one is running: it was broadcast once, at the
	// start of the round, which this client wasn't around for.
	if (m_flRoundTimeLimit != 0.0f)
		SendRoundTimer(pPlayer->edict());

	// And the role label - a blank for a joiner, but a reconnecting client
	// may still be showing the label from its last visit.
	SendRoleHud(pPlayer);

	// And the loot count, which is what puts the loot readout on the HUD in
	// place of the armour one - even a zero.
	SendLootCount(pPlayer);

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pOther = GetPlayerByIndex(i);
		if (!pOther)
			continue;

		SendSpectatorState(pOther, IsSittingOut(i), pPlayer->edict());
	}
}

void CHalfLifeCrowbarHunt::UpdateGameMode(CBasePlayer* pPlayer)
{
	MESSAGE_BEGIN(MSG_ONE, gmsgGameMode, NULL, pPlayer->edict());
	WRITE_BYTE(CH_GAMEMODE_CROWBARHUNT);
	MESSAGE_END();
}

void CHalfLifeCrowbarHunt::SendSpectatorState(CBasePlayer* pPlayer, bool bObserver, edict_t* pTarget)
{
	if (pTarget)
		MESSAGE_BEGIN(MSG_ONE, gmsgSpectator, NULL, pTarget);
	else
		MESSAGE_BEGIN(MSG_ALL, gmsgSpectator);
	WRITE_BYTE(ENTINDEX(pPlayer->edict()));
	WRITE_BYTE(bObserver ? 1 : 0);
	MESSAGE_END();
}

void CHalfLifeCrowbarHunt::ServiceSpectatorState(CBasePlayer* pPlayer)
{
	const int index = ENTINDEX(pPlayer->edict());
	// Only those who asked to sit out (the "spectate" command). The round's dead
	// and sidelined joiners are observers too, but they are still players in
	// the round and belong under the Players header.
	const int observer = IsSittingOut(index) ? 1 : 0;

	if (m_iSentSpectator[index] == observer)
		return;

	m_iSentSpectator[index] = observer;
	SendSpectatorState(pPlayer, observer != 0, NULL);
}

// Snapshot a player's body into a standalone entity, so it stays visible after
// StartObserver() makes the player themselves disappear.
void CHalfLifeCrowbarHunt::LeaveCorpse(CBasePlayer* pPlayer)
{
	// A gibbed player has had pev->model cleared - there is no body to leave.
	if (FStringNull(pPlayer->pev->model))
		return;

	CBaseEntity* pCorpse = CBaseEntity::Create("ch_corpse", pPlayer->pev->origin, pPlayer->pev->angles);

	if (!pCorpse)
		return;

	entvars_t* pev = pCorpse->pev;

	SET_MODEL(ENT(pev), STRING(pPlayer->pev->model));

	pev->movetype = MOVETYPE_NONE; // the body has already finished falling
	pev->solid    = SOLID_NOT;     // never block a corridor or a crowbar swing
	pev->takedamage = DAMAGE_NO;

	// Everything that makes this body look like the player it came from is
	// picked up client-side, in StudioDrawCorpse(): kRenderFxCHCorpse tells the
	// renderer what this is, and renderamt carries the slot it needs to ask the
	// engine for that player's model. Neither can be done from here - the model
	// a player chose exists only in their userinfo, and the server puts every
	// player in "models/player.mdl" regardless.
	pev->renderfx = kRenderFxCHCorpse;
	pev->renderamt = ENTINDEX(pPlayer->edict());

	// Colours, though, we snapshot: packed low byte top, high byte bottom, the
	// packing both studio draw paths unpack. Reading them here rather than
	// letting the client read the live ones freezes the body as it was at the
	// moment of death, so it can't restyle itself later and give its owner away.
	pev->colormap = PlayerRemapColor(pPlayer, "topcolor") | (PlayerRemapColor(pPlayer, "bottomcolor") << 8);

	pev->skin = pPlayer->pev->skin;
	pev->body = pPlayer->pev->body;

	// Freeze on the frame the death animation ended on. PlayerDeathThink() has
	// already run StopAnimation(), so pev->frame is the pose we want.
	pev->sequence  = pPlayer->pev->sequence;
	pev->frame     = pPlayer->pev->frame;
	pev->animtime  = gpGlobals->time;
	pev->framerate = 0;

	UTIL_SetSize(pev, pPlayer->pev->mins, pPlayer->pev->maxs);
	UTIL_SetOrigin(pev, pPlayer->pev->origin);
}

// ---------------------------------------------------------------------------
// Death handling / win conditions
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::PlayerKilled(CBasePlayer* pVictim, entvars_t* pKiller, entvars_t* pInflictor)
{
	// A 200-damage hit leaves the corpse at around -100 health, and
	// CBasePlayer::Killed() gibs anything under -40. We run before that check
	// (and before deadflag is set), so pulling the overkill back to 0 here
	// keeps the one-shot kill without the gore. 0 still reads as dead for
	// IsAlive(), so the counts below are unaffected.
	if (pVictim && pVictim->pev->health < 0)
		pVictim->pev->health = 0;

	CHalfLifeMultiplay::PlayerKilled(pVictim, pKiller, pInflictor);

	// DeadPlayerWeapons() is GR_PLR_DROP_GUN_NO, so the corpse drops nothing on
	// its own - see the note there. The Hunter's revolver is the exception: it
	// is meant to be inheritable, so it is dropped by hand here, into a box of
	// its own. Doing it this way also fixes the case the inherited
	// GR_PLR_DROP_GUN_ACTIVE got wrong, where a Hunter killed while holstered
	// dropped their empty hands and took the revolver out of the round with
	// them. We run from CBasePlayer::Killed(), well before PackDeadPlayerItems()
	// (deferred to PlayerDeathThink()), so the gun is out of the inventory in
	// time and its ammo goes into the box with it.
	if (pVictim && GetPlayerRole(pVictim) == CHRole::Hunter && pVictim->HasNamedPlayerItem("weapon_357"))
	{
		CH_DropRevolver(pVictim);
	}

	// The victim's health is already <= 0 here (CBasePlayer::Killed() calls us
	// before it sets deadflag), so IsAlive() reports false and the counts
	// below are correct. Handing them to observer mode has to wait until
	// their death animation finishes - Think() picks that up.
	if (m_roundState == CHRoundState::InProgress)
		CheckRoundWinConditions();
}

// ---------------------------------------------------------------------------
// Obituaries
//
// The stock killfeed is the mode's largest information leak: a single crowbar
// obituary names the Killer to the whole server the moment they swing. The HUD
// draws that feed purely from gmsgDeathMsg, so this override never sends it -
// no death produces a killfeed entry at all.
//
// Kills that are *not* the Killer's are public knowledge by design (a Hunter
// shooting the wrong person is the mistake the mode is built around, and it
// already costs them the revolver), so those are announced in chat instead.
// Suicides and world deaths stay silent: they say nothing about who is who.
//
// Deliberately does not chain to CHalfLifeMultiplay::DeathNotice() - that
// always writes gmsgDeathMsg and there is no taking it back from here. The
// parts of it worth keeping, the server log line and the HLTV director event,
// are reproduced below.
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::DeathNotice(CBasePlayer* pVictim, entvars_t* pKiller, entvars_t* pInflictor)
{
	if (!pVictim || !pKiller)
		return;

	const bool bKillerIsPlayer = (pKiller->flags & FL_CLIENT) != 0;

	// Same resolution the base class does, for the log line below.
	const char* killer_weapon_name = "world";

	if (bKillerIsPlayer)
	{
		if (pInflictor)
		{
			if (pInflictor == pKiller)
			{
				CBasePlayer* pPlayer = static_cast<CBasePlayer*>(CBaseEntity::Instance(pKiller));

				if (pPlayer && pPlayer->m_pActiveItem)
					killer_weapon_name = pPlayer->m_pActiveItem->pszName();
			}
			else
			{
				killer_weapon_name = STRING(pInflictor->classname);
			}
		}
	}
	else if (pInflictor)
	{
		killer_weapon_name = STRING(pInflictor->classname);
	}

	if (strncmp(killer_weapon_name, "weapon_", 7) == 0)
		killer_weapon_name += 7;
	else if (strncmp(killer_weapon_name, "monster_", 8) == 0)
		killer_weapon_name += 8;
	else if (strncmp(killer_weapon_name, "func_", 5) == 0)
		killer_weapon_name += 5;

	// The announcement itself: somebody other than the Killer killed somebody
	// other than themselves. Only the attacker is named - naming the victim, or
	// saying what killed them, would hand the room a free read on who just went
	// quiet and what they were holding. That someone innocent died is the whole
	// message - or, if the victim was the Killer, that the round has just been
	// won and by whom. The round result itself follows a beat later, from
	// AnnounceRoundOver().
	if (bKillerIsPlayer && pKiller != pVictim->pev)
	{
		CBasePlayer* pAttacker = static_cast<CBasePlayer*>(CBaseEntity::Instance(pKiller));

		if (pAttacker && GetPlayerRole(pAttacker) != CHRole::Killer)
		{
			const bool bKilledTheKiller = GetPlayerRole(pVictim) == CHRole::Killer;

			// Sent as SayText rather than a plain print so the name is drawn in
			// the shooter's own colour: saytext.cpp only colours a name when
			// the line opens with \2, and it looks the colour up per client
			// index. Under ch_anonymous that is the colour they are wearing,
			// which is the only handle anyone in the room has on who this was.
			char szText[128];
			snprintf(szText, sizeof(szText), "\2%s killed %s.\n",
				STRING(pAttacker->pev->netname),
				bKilledTheKiller ? "the Killer" : "an innocent survivor");

			MESSAGE_BEGIN(MSG_ALL, gmsgSayText, nullptr);
			WRITE_BYTE(pAttacker->entindex());
			WRITE_STRING(szText);
			MESSAGE_END();
		}
	}

	// Server log, in the non-teamplay format - this mode is never teamplay.
	if (pVictim->pev == pKiller)
	{
		UTIL_LogPrintf("\"%s<%i><%s><%i>\" committed suicide with \"%s\"\n",
			STRING(pVictim->pev->netname),
			GETPLAYERUSERID(pVictim->edict()),
			GETPLAYERAUTHID(pVictim->edict()),
			GETPLAYERUSERID(pVictim->edict()),
			killer_weapon_name);
	}
	else if (bKillerIsPlayer)
	{
		UTIL_LogPrintf("\"%s<%i><%s><%i>\" killed \"%s<%i><%s><%i>\" with \"%s\"\n",
			STRING(pKiller->netname),
			GETPLAYERUSERID(ENT(pKiller)),
			GETPLAYERAUTHID(ENT(pKiller)),
			GETPLAYERUSERID(ENT(pKiller)),
			STRING(pVictim->pev->netname),
			GETPLAYERUSERID(pVictim->edict()),
			GETPLAYERAUTHID(pVictim->edict()),
			GETPLAYERUSERID(pVictim->edict()),
			killer_weapon_name);
	}
	else
	{
		UTIL_LogPrintf("\"%s<%i><%s><%i>\" committed suicide with \"%s\" (world)\n",
			STRING(pVictim->pev->netname),
			GETPLAYERUSERID(pVictim->edict()),
			GETPLAYERAUTHID(pVictim->edict()),
			GETPLAYERUSERID(pVictim->edict()),
			killer_weapon_name);
	}

	// HLTV director hint, unchanged from the base class.
	MESSAGE_BEGIN(MSG_SPEC, SVC_DIRECTOR);
	WRITE_BYTE(9);							 // command length in bytes
	WRITE_BYTE(DRC_CMD_EVENT);				 // player killed
	WRITE_SHORT(ENTINDEX(pVictim->edict())); // index number of primary entity
	if (pInflictor)
		WRITE_SHORT(ENTINDEX(ENT(pInflictor))); // index number of secondary entity
	else
		WRITE_SHORT(ENTINDEX(ENT(pKiller))); // index number of secondary entity
	WRITE_LONG(7 | DRC_FLAG_DRAMATIC);		 // eventflags (priority and flags)
	MESSAGE_END();
}

void CHalfLifeCrowbarHunt::CheckRoundWinConditions()
{
	int killersAlive   = CountAlivePlayersWithRole(CHRole::Killer);
	int survivorsAlive = CountAlivePlayersWithRole(CHRole::Hunter)
	                    + CountAlivePlayersWithRole(CHRole::Survivor);

	if (killersAlive <= 0)
	{
		EndRound(CHRole::Survivor); // Killer died - Hunter/Survivors win
	}
	else if (survivorsAlive <= 0)
	{
		EndRound(CHRole::Killer); // everyone else is dead - Killer wins
	}
	// else: round continues, nothing to do yet
}
