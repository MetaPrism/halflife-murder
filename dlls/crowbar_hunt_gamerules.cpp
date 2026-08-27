#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
#include "crowbar_hunt_gamerules.h"

CHalfLifeCrowbarHunt::CHalfLifeCrowbarHunt()
{
	m_roundState        = CHRoundState::WaitingForPlayers;
	m_flStateEnterTime  = gpGlobals->time;
	m_flPreRoundLength  = 5.0f;
	m_flRoundEndLength  = 8.0f;

	for (int i = 0; i < 33; i++)
		m_playerRoles[i] = CHRole::Unassigned;
}

// ---------------------------------------------------------------------------
// Called once per server frame (via g_pGameRules->Think() in the engine loop)
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::Think()
{
	// Let the base class keep handling generic multiplayer bookkeeping.
	// Remove this call if it fights with your own round timer/HUD messages.
	CHalfLifeMultiplay::Think();

	switch (m_roundState)
	{
	case CHRoundState::WaitingForPlayers:
		// TODO: pick your own player-count threshold, ideally a cvar
		if (CountConnectedPlayers() >= 2)
			StartPreRound();
		break;

	case CHRoundState::PreRound:
		if (gpGlobals->time - m_flStateEnterTime >= m_flPreRoundLength)
			StartRound();
		break;

	case CHRoundState::InProgress:
		CheckRoundWinConditions();
		break;

	case CHRoundState::RoundEnd:
		if (gpGlobals->time - m_flStateEnterTime >= m_flRoundEndLength)
			RestartMap();
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

	// TODO: respawn everyone now so they're standing at spawn points during
	// the countdown, with no weapons yet (PlayerSpawn() already withholds
	// weapons outside of InProgress, see below).
	UTIL_ClientPrintAll(HUD_PRINTCENTER, "Roles assigned. Get ready...\n");
}

void CHalfLifeCrowbarHunt::StartRound()
{
	SetRoundState(CHRoundState::InProgress);

	// TODO: iterate all connected CBasePlayer instances and call
	// GiveRoleLoadout(pPlayer, GetPlayerRole(pPlayer)) here, so weapons show
	// up the instant the round goes live rather than at spawn time.
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

void CHalfLifeCrowbarHunt::RestartMap()
{
	for (int i = 0; i < 33; i++)
		m_playerRoles[i] = CHRole::Unassigned;

	SetRoundState(CHRoundState::WaitingForPlayers);
	// TODO: force-respawn all connected players so PlayerSpawn() re-fires
	// and they lose their old-round weapons.
}

// ---------------------------------------------------------------------------
// Role assignment
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::AssignRoles()
{
	for (int i = 0; i < 33; i++)
		m_playerRoles[i] = CHRole::Survivor;

	CBasePlayer* pKiller = PickRandomAlivePlayer();
	if (pKiller)
		SetPlayerRole(pKiller, CHRole::Killer);

	// Pick the Hunter from everyone except whoever just got Killer.
	CBasePlayer* pHunter = PickRandomAlivePlayer(CHRole::Killer);
	if (pHunter)
		SetPlayerRole(pHunter, CHRole::Hunter);

	// TODO: tell the Hunter privately that they're the Hunter (ClientPrint
	// to just that one player's edict) instead of broadcasting it - part of
	// the fun is the Killer not knowing who's armed.
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
		// TODO: verify the ammo name ("357") and max-carry constant against
		// this repo's weapons.h - names sometimes differ from vanilla HLSDK.
		pPlayer->GiveAmmo(6, "357", _357_MAX_CARRY);
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
	if (idx < 0 || idx >= 33)
		return;

	m_playerRoles[idx] = role;
}

CHRole CHalfLifeCrowbarHunt::GetPlayerRole(CBasePlayer* pPlayer) const
{
	if (!pPlayer)
		return CHRole::Unassigned;

	int idx = ENTINDEX(pPlayer->edict());
	if (idx < 0 || idx >= 33)
		return CHRole::Unassigned;

	return m_playerRoles[idx];
}

int CHalfLifeCrowbarHunt::CountConnectedPlayers() const
{
	int count = 0;

	for (int i = 1; i <= gpGlobals->maxClients; i++)
	{
		CBaseEntity* pEnt = UTIL_PlayerByIndex(i);

		if (!pEnt)
			continue;

		count++;
	}

	return count;
}

int CHalfLifeCrowbarHunt::CountAlivePlayersWithRole(CHRole role) const
{
	int count = 0;
	// TODO: replace with a real "iterate connected players" loop, e.g.:
	//
	//   for (int i = 1; i <= gpGlobals->maxClients; i++)
	//   {
	//       CBaseEntity* pEnt = UTIL_PlayerByIndex(i);
	//       if (!pEnt) continue;
	//       CBasePlayer* pPlayer = static_cast<CBasePlayer*>(pEnt);
	//       if (!pPlayer->IsAlive()) continue;
	//       if (m_playerRoles[i] == role) count++;
	//   }
	//
	// (function names above are illustrative - match them to what this repo
	// actually exposes in util.h.)
	return count;
}

CBasePlayer* CHalfLifeCrowbarHunt::PickRandomAlivePlayer(CHRole excludeRole) const
{
	// TODO: build a small array/vector of alive CBasePlayer* (skipping
	// anyone who already holds excludeRole), then return
	// list[RANDOM_LONG(0, list.size() - 1)]. RANDOM_LONG is the engine's
	// seeded RNG helper, declared in util.h.
	return nullptr;
}

// ---------------------------------------------------------------------------
// Spawning / respawning
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::PlayerSpawn(CBasePlayer* pPlayer)
{
	CHalfLifeMultiplay::PlayerSpawn(pPlayer);

	CHRole role = GetPlayerRole(pPlayer);

	if (m_roundState == CHRoundState::InProgress)
		GiveRoleLoadout(pPlayer, role);
	else
		pPlayer->RemoveAllItems(false); // no weapons while waiting/pre-round
}

bool CHalfLifeCrowbarHunt::FPlayerCanRespawn(CBasePlayer* pPlayer)
{
	// No mid-round respawning - once you're dead, you spectate until the
	// round ends. Only allow respawns between rounds.
	return m_roundState != CHRoundState::InProgress;
}

// ---------------------------------------------------------------------------
// Death handling / win conditions
// ---------------------------------------------------------------------------
void CHalfLifeCrowbarHunt::PlayerKilled(CBasePlayer* pVictim, entvars_t* pKiller, entvars_t* pInflictor)
{
	CHalfLifeMultiplay::PlayerKilled(pVictim, pKiller, pInflictor);

	// TODO: drop the victim into observer/spectator mode immediately
	// (there's usually a CBasePlayer::StartObserver()-style call, or you
	// set pev->deadflag / move type by hand) rather than just leaving the
	// corpse and waiting on FPlayerCanRespawn.

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
