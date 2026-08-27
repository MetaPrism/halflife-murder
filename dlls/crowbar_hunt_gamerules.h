/*
===============================================================================
Crowbar Hunt game mode - round-based "one killer vs the rest" gamerules.

One player (the "Killer") spawns with only a crowbar and must eliminate
everyone else. One other player (the "Hunter") is secretly given a revolver
(weapon_357) to try to stop the Killer. Everyone else (the "Survivors")
spawns unarmed and has to run/hide/rely on the Hunter.

This is a SKELETON / starting point, not a finished drop-in mode. Search for
"TODO" for the spots you'll want to flesh out, and double-check every
function name/signature below against this repo's actual gamerules.h /
player.h / weapons.h - the names used here follow the traditional HLSDK
CGameRules interface, which "Half-Life Updated" may have tweaked slightly.
===============================================================================
*/
#pragma once

#include "multiplay_gamerules.h" // CHalfLifeMultiplay - reuse its respawn/HUD plumbing

// Role a player currently holds for the round
enum class CHRole
{
	Unassigned = 0,
	Spectator,
	Killer,
	Hunter,
	Survivor,
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
	BOOL        FPlayerCanRespawn(CBasePlayer* pPlayer) override;
	void        PlayerKilled(CBasePlayer* pVictim, entvars_t* pKiller, entvars_t* pInflictor) override;
	const char* GetGameDescription() override { return "Crowbar Hunt"; }
	BOOL        IsMultiplayer() override { return TRUE; }
	BOOL        IsDeathmatch() override { return TRUE; }
	BOOL        IsCoOp() override { return FALSE; }

private:
	// --- round flow ---
	void SetRoundState(CHRoundState state);
	void StartPreRound();
	void StartRound();
	void EndRound(CHRole winningRole);
	void RestartMap(); // reset roles, go back to WaitingForPlayers/PreRound

	// --- role management ---
	void   AssignRoles();
	void   GiveRoleLoadout(CBasePlayer* pPlayer, CHRole role);
	void   SetPlayerRole(CBasePlayer* pPlayer, CHRole role);
	CHRole GetPlayerRole(CBasePlayer* pPlayer) const;

	int          CountAlivePlayersWithRole(CHRole role) const;
	CBasePlayer* PickRandomAlivePlayer(CHRole excludeRole = CHRole::Unassigned) const;

	void CheckRoundWinConditions();

	CHRoundState m_roundState;
	float        m_flStateEnterTime; // gpGlobals->time when we entered m_roundState

	// how long each phase lasts, in seconds - tune to taste / expose as cvars
	float m_flPreRoundLength;
	float m_flRoundEndLength;

	// role assigned to each possible player slot, indexed by ENTINDEX() (1..32)
	CHRole m_playerRoles[33];
};
