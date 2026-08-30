#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "client.h"
#include "skill.h"
#include "crowbar_hunt_gamerules.h"

// Minimum number of connected players before a round will start.
// TODO: expose as a cvar once there's a reason to tune it per server.
constexpr int CH_MIN_PLAYERS = 2;

// Both role weapons are one-hit kills: the round is decided by positioning and
// who shoots first, not by trading damage.
constexpr float CH_CROWBAR_DAMAGE = 200.0f;
constexpr float CH_357_DAMAGE = 200.0f;

namespace
{
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
	"weaponbox",
	"grenade",
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
} // namespace

CHalfLifeCrowbarHunt::CHalfLifeCrowbarHunt()
{
	m_roundState        = CHRoundState::WaitingForPlayers;
	m_flStateEnterTime  = gpGlobals->time;
	m_flPreRoundLength  = 5.0f;
	m_flRoundEndLength  = 8.0f;
	m_numSnapshots      = 0;
	m_bSnapshotTaken    = false;

	for (int i = 0; i <= MAX_PLAYERS; i++)
		m_playerRoles[i] = CHRole::Unassigned;

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

	// First frame after the map finished spawning its entities: record what
	// everything looked like before anyone can break or open it, and clear the
	// map's guns out straight away.
	if (!m_bSnapshotTaken)
		ResetMapEntities();

	switch (m_roundState)
	{
	case CHRoundState::WaitingForPlayers:
		if (CountConnectedPlayers() >= CH_MIN_PLAYERS)
			StartPreRound();
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
		break;

	case CHRoundState::RoundEnd:
		// Keep parking the dead in observer mode - the death that ended the
		// round happens before its animation finishes, so the last victim
		// still needs picking up. ResetForNextRound() takes them back out.
		MoveDeadPlayersToObserver();

		if (gpGlobals->time - m_flStateEnterTime >= m_flRoundEndLength)
			ResetForNextRound();
		break;
	}
}

void CHalfLifeCrowbarHunt::SetRoundState(CHRoundState state)
{
	m_roundState       = state;
	m_flStateEnterTime = gpGlobals->time;
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
	}

	UTIL_ClientPrintAll(HUD_PRINTCENTER, "Round started! Survive... or hunt.\n");
}

void CHalfLifeCrowbarHunt::EndRound(CHRole winningRole)
{
	SetRoundState(CHRoundState::RoundEnd);

	const char* msg = "Round over.\n";
	switch (winningRole)
	{
	case CHRole::Killer:
		msg = "The Killer wins the round!\n";
		break;
	case CHRole::Hunter:
	case CHRole::Survivor:
		msg = "The Survivors win the round!\n";
		break;
	default:
		break;
	}
	UTIL_ClientPrintAll(HUD_PRINTCENTER, msg);
}

void CHalfLifeCrowbarHunt::ResetForNextRound()
{
	for (int i = 0; i <= MAX_PLAYERS; i++)
		m_playerRoles[i] = CHRole::Unassigned;

	SetRoundState(CHRoundState::WaitingForPlayers);

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

// Sweep up what the last round left lying around, and keep the map's own guns
// out of play - a Survivor picking up an MP5 would undo the whole premise.
void CHalfLifeCrowbarHunt::RemoveRoundLitter()
{
	for (int i = gpGlobals->maxClients + 1; i < gpGlobals->maxEntities; i++)
	{
		edict_t* pEdict = INDEXENT(i);

		if (!pEdict || 0 != pEdict->free || FStringNull(pEdict->v.classname))
			continue;

		const char* pszClassname = STRING(pEdict->v.classname);

		// A weapon or ammo entity someone is carrying has its owner set, so
		// only the ones lying in the world get taken.
		const bool isLooseWeapon = FNullEnt(pEdict->v.owner) && (0 == strncmp(pszClassname, "weapon_", 7) || 0 == strncmp(pszClassname, "ammo_", 5));

		if (!isLooseWeapon && !ClassnameInList(pszClassname, g_szLitterClassnames))
			continue;

		CBaseEntity* pEntity = CBaseEntity::Instance(pEdict);

		if (pEntity)
			UTIL_Remove(pEntity);
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
	for (int i = 0; i <= MAX_PLAYERS; i++)
		m_playerRoles[i] = CHRole::Unassigned;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (!pPlayer)
			continue;

		m_playerRoles[i] = CHRole::Survivor;
	}

	CBasePlayer* pKiller = PickRandomAlivePlayer();
	if (pKiller)
		SetPlayerRole(pKiller, CHRole::Killer);

	// Pick the Hunter from everyone except whoever just got Killer.
	CBasePlayer* pHunter = PickRandomAlivePlayer(CHRole::Killer);
	if (pHunter)
		SetPlayerRole(pHunter, CHRole::Hunter);
}

// Tell one player what they are. Deliberately never broadcast - half the fun
// is the Killer not knowing who's carrying the revolver.
void CHalfLifeCrowbarHunt::AnnounceRole(CBasePlayer* pPlayer, CHRole role) const
{
	if (!pPlayer)
		return;

	switch (role)
	{
	case CHRole::Killer:
		ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "You are the KILLER.\nHunt them down.\n");
		break;

	case CHRole::Hunter:
		ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "You are the HUNTER.\nYou have the revolver - find the Killer.\n");
		break;

	case CHRole::Survivor:
		ClientPrint(pPlayer->pev, HUD_PRINTCENTER, "You are a SURVIVOR.\nStay alive.\n");
		break;

	default:
		break;
	}
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

	if (!pPlayer || !pPlayer->IsAlive() || m_roundState != CHRoundState::InProgress)
		return;

	// Only for whoever is actually carrying the revolver - a Survivor who picks
	// it up off the dead Hunter inherits the endless reserve with it.
	if (pPlayer->ammo_357 < _357_MAX_CARRY && pPlayer->HasNamedPlayerItem("weapon_357"))
		pPlayer->ammo_357 = _357_MAX_CARRY;
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

	if (pPlayer && pItem && !RoleCanCarryWeapon(GetPlayerRole(pPlayer), STRING(pItem->pev->classname)))
		return false;

	return CHalfLifeMultiplay::CanHavePlayerItem(pPlayer, pItem);
}

void CHalfLifeCrowbarHunt::GiveRoleLoadout(CBasePlayer* pPlayer, CHRole role)
{
	if (!pPlayer)
		return;

	pPlayer->RemoveAllItems(false); // strip default HEV/suit weapons first

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
		// Unarmed on purpose. TODO: maybe give a flashlight-only suit, or a
		// sprint/stamina mechanic to help them evade the Killer.
		break;
	}
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
	// UTIL_PlayerByIndex() returns null for slots nobody is connected to.
	return static_cast<CBasePlayer*>(UTIL_PlayerByIndex(index));
}

int CHalfLifeCrowbarHunt::CountConnectedPlayers() const
{
	int count = 0;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		if (GetPlayerByIndex(i))
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

	// Deliberately not CHalfLifeMultiplay::PlayerSpawn(). That hands every
	// spawning player a crowbar and a glock, and GiveNamedItem() delivers them
	// by spawning a real world entity at the player's feet and touching them
	// with it. When CanHavePlayerItem() refuses - which it does for every
	// spawn outside a live round - the entity is not removed, it just stays
	// lying on the spawn point as a free pickup. Loadouts come from
	// GiveRoleLoadout() instead, so the default equip is never wanted here.
	pPlayer->SetHasSuit(true);

	CHRole role = GetPlayerRole(pPlayer);

	if (m_roundState != CHRoundState::InProgress)
	{
		pPlayer->RemoveAllItems(false); // no weapons while waiting/pre-round
		return;
	}

	if (role == CHRole::Unassigned)
	{
		// Connected after roles were handed out - sit this round out rather
		// than joining as a free extra Survivor.
		pPlayer->RemoveAllItems(false);
		pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->v_angle);
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

void CHalfLifeCrowbarHunt::ForceRespawnAllPlayers() const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
		ForceRespawn(GetPlayerByIndex(i));
}

void CHalfLifeCrowbarHunt::ForceRespawnDeadPlayers() const
{
	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBasePlayer* pPlayer = GetPlayerByIndex(i);

		if (pPlayer && !pPlayer->IsAlive())
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

		pPlayer->StartObserver(pPlayer->pev->origin, pPlayer->pev->v_angle);
	}
}

// ---------------------------------------------------------------------------
// Death handling / win conditions
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::PlayerKilled(CBasePlayer* pVictim, entvars_t* pKiller, entvars_t* pInflictor)
{
	CHalfLifeMultiplay::PlayerKilled(pVictim, pKiller, pInflictor);

	// The victim's health is already <= 0 here (CBasePlayer::Killed() calls us
	// before it sets deadflag), so IsAlive() reports false and the counts
	// below are correct. Handing them to observer mode has to wait until
	// their death animation finishes - Think() picks that up.
	if (m_roundState == CHRoundState::InProgress)
		CheckRoundWinConditions();
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
