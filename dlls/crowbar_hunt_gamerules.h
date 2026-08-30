/*
===============================================================================
Crowbar Hunt game mode - round-based "one killer vs the rest" gamerules.

One player (the "Killer") spawns with only a crowbar and must eliminate
everyone else. One other player (the "Hunter") is secretly given a revolver
(weapon_357) to try to stop the Killer. Everyone else (the "Survivors")
spawns unarmed and has to run/hide/rely on the Hunter.

Selected at map load by InstallGameRules() when "sv_crowbarhunt 1" is set on
a deathmatch server.
===============================================================================
*/
#pragma once

#include "cdll_dll.h"	// MAX_PLAYERS
#include "gamerules.h" // CHalfLifeMultiplay - reuse its respawn/HUD plumbing

// Role a player currently holds for the round
enum class CHRole
{
	Unassigned = 0,
	Spectator,
	Killer,
	Hunter,
	Survivor,
};

// Maximum number of map entities whose spawn state we track for round resets.
// Comfortably above what a Half-Life deathmatch map uses.
#define CH_MAX_TRACKED_ENTITIES 512

// Spawn-time state of one resettable map entity (breakable, door, button), so
// a round reset can put it back without reloading the level.
struct CHEntitySnapshot
{
	EHANDLE  hEntity;
	Vector   vecOrigin;
	Vector   vecAngles;
	float    flHealth;
	float    flTakeDamage;
	float    flFrame;
	int      iSolid;
	int      iMoveType;
	int      iEffects;
	int      iToggleState; // TOGGLE_STATE at spawn, -1 if not a CBaseToggle
	string_t iszTargetName;

	// Die()/Killed() and the door/button move code all rewire these, so they
	// have to come back too or a reset entity stops responding to touch/use.
	void (CBaseEntity::*pfnThink)();
	void (CBaseEntity::*pfnTouch)(CBaseEntity* pOther);
	void (CBaseEntity::*pfnUse)(CBaseEntity* pActivator, CBaseEntity* pCaller, USE_TYPE useType, float value);
};

// Which phase the round is currently in
enum class CHRoundState
{
	WaitingForPlayers, // not enough players connected yet
	PreRound,          // countdown before weapons/roles go live
	InProgress,        // round is live, win conditions are being checked
	RoundEnd,          // showing round result, waiting to restart
};

class CHalfLifeCrowbarHunt : public CHalfLifeMultiplay
{
public:
	CHalfLifeCrowbarHunt();

	// --- CGameRules overrides ---
	void        Think() override;
	void        PlayerSpawn(CBasePlayer* pPlayer) override;
	void        PlayerThink(CBasePlayer* pPlayer) override;

	// CHalfLifeMultiplay hardcodes deathmatch damage over whatever skill.cfg
	// says, so this mode has to have the last word on its two weapons.
	void        RefreshSkillData() override;
	bool        FPlayerCanRespawn(CBasePlayer* pPlayer) override;
	void        PlayerKilled(CBasePlayer* pVictim, entvars_t* pKiller, entvars_t* pInflictor) override;

	// Weapons belong to roles: the crowbar marks the Killer and the revolver
	// the Hunter, so neither can be traded across that line by picking one up.
	bool CanHavePlayerItem(CBasePlayer* pPlayer, CBasePlayerItem* pItem) override;
	const char* GetGameDescription() override { return "Crowbar Hunt"; }

	// Broken brush entities have to survive until the next round reset can put
	// them back, so CBreakable::Die() must not free their edict.
	bool ShouldPreserveBrokenEntities() override { return true; }

	bool        IsMultiplayer() override { return true; }
	bool        IsDeathmatch() override { return true; }
	bool        IsCoOp() override { return false; }

private:
	// --- round flow ---
	void SetRoundState(CHRoundState state);
	void StartPreRound();
	void StartRound();
	void EndRound(CHRole winningRole);
	void ResetForNextRound(); // reset roles, go back to WaitingForPlayers/PreRound

	// --- map reset ---
	void        TakeMapSnapshot();  // record spawn state of resettable entities (once per map)
	void        ResetMapEntities(); // put the world back the way the map loaded
	static void RemoveRoundLitter(); // gibs, corpsebags, live ordnance, map guns

	// --- role management ---
	void   AssignRoles();
	void   GiveRoleLoadout(CBasePlayer* pPlayer, CHRole role);
	void   SetPlayerRole(CBasePlayer* pPlayer, CHRole role);
	CHRole GetPlayerRole(CBasePlayer* pPlayer) const;
	void   AnnounceRole(CBasePlayer* pPlayer, CHRole role) const;

	// Is a weapon classname one this role is allowed to carry?
	static bool RoleCanCarryWeapon(CHRole role, const char* pszWeaponName);

	// --- player iteration ---
	// Returns the connected player in slot "index" (1..maxClients), or null.
	static CBasePlayer* GetPlayerByIndex(int index);

	int          CountConnectedPlayers() const;
	int          CountAlivePlayersWithRole(CHRole role) const;
	CBasePlayer* PickRandomAlivePlayer(CHRole excludeRole = CHRole::Unassigned) const;

	// --- spawn/death plumbing ---
	void ForceRespawn(CBasePlayer* pPlayer) const;
	void StripAllPlayers() const; // take every weapon off every connected player
	void ForceRespawnAllPlayers() const;
	void ForceRespawnDeadPlayers() const;
	void MoveDeadPlayersToObserver() const;

	void CheckRoundWinConditions();

	CHRoundState m_roundState;
	float        m_flStateEnterTime; // gpGlobals->time when we entered m_roundState

	// how long each phase lasts, in seconds - tune to taste / expose as cvars
	float m_flPreRoundLength;
	float m_flRoundEndLength;

	// role assigned to each possible player slot, indexed by ENTINDEX() (1..MAX_PLAYERS)
	CHRole m_playerRoles[MAX_PLAYERS + 1];

	// Spawn-time state of every resettable map entity, taken once on the first
	// frame after the map has finished spawning its entities.
	CHEntitySnapshot m_mapSnapshot[CH_MAX_TRACKED_ENTITIES];
	int              m_numSnapshots;
	bool             m_bSnapshotTaken;
};
