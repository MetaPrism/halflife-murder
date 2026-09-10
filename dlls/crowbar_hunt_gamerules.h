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
#include "crowbar_hunt_shared.h" // the anonymous-mode colour table

// Role a player currently holds for the round
enum class CHRole
{
	Unassigned = 0,
	Spectator,
	Killer,
	Hunter,
	Survivor,
};

// How often, in seconds, WaitingForPlayers reminds the server it's waiting.
#define CH_WAITING_ANNOUNCE_INTERVAL 30.0f

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

	// The standard killfeed would out the Killer the moment they swing, so it
	// is suppressed entirely and replaced with a chat line for the kills that
	// are not the Killer's - see the comment on the implementation.
	void        DeathNotice(CBasePlayer* pVictim, entvars_t* pKiller, entvars_t* pInflictor) override;

	// Weapons belong to roles: the crowbar marks the Killer and the revolver
	// the Hunter, so neither can be traded across that line by picking one up.
	bool CanHavePlayerItem(CBasePlayer* pPlayer, CBasePlayerItem* pItem) override;

	// The weapon gate above is not enough on its own: a weaponbox hands its
	// ammo over before it asks about its weapons, so ammo needs the same say.
	bool CanHaveAmmo(CBasePlayer* pPlayer, const char* pszAmmoName, int iMaxCarry) override;

	// Nobody drops a weaponbox. Everyone in this mode carries weapon_hands, and
	// with the inherited GR_PLR_DROP_GUN_ACTIVE that is usually what the corpse
	// would drop: a box full of nothing, sitting there looking like loot. With
	// both rules set to NO, PackDeadPlayerItems() takes its early-out and never
	// creates a box at all. The one drop that matters - the dead Hunter's
	// revolver - is made explicitly in PlayerKilled() instead.
	int DeadPlayerWeapons(CBasePlayer* pPlayer) override { return GR_PLR_DROP_GUN_NO; }
	int DeadPlayerAmmo(CBasePlayer* pPlayer) override { return GR_PLR_DROP_AMMO_NO; }

	// And no dropping by hand either. Loadouts are the role: a Killer who drops
	// the crowbar stops reading as the Killer, a Hunter can hand the revolver to
	// a Survivor the Killer is already chasing, and anyone can drop their empty
	// hands and leave a box on the floor that means nothing. The one drop the
	// mode wants - a dead Hunter's revolver - is made in code, which this does
	// not touch. weapon_hands is the reason this can't simply be a per-weapon
	// rule: every role carries it, so there is nothing left to allow.
	bool AllowPlayerDropCommand(CBasePlayer* pPlayer) override { return false; }
	const char* GetGameDescription() override { return "Crowbar Hunt"; }

	// Broken brush entities have to survive until the next round reset can put
	// them back, so CBreakable::Die() must not free their edict.
	bool ShouldPreserveBrokenEntities() override { return true; }

	// Proximity voice chat, when ch_proxvoice is on: hiding is the Survivors'
	// main defence, so a voice that carries the whole map would give them away.
	bool CanPlayerHearPlayer(CBasePlayer* pListener, CBasePlayer* pTalker) override;

	// The Hunter is never told who the Killer is, so nothing but a penalty
	// stops them shooting on suspicion and reading the round off the bodies.
	// This is where a shot at anyone but the Killer is caught and charged for.
	bool FPlayerCanTakeDamage(CBasePlayer* pPlayer, CBaseEntity* pAttacker) override;

	// Anonymous mode has to have the last word on a player's name and colours:
	// a player who retypes either mid-round would otherwise hand the room a
	// free read on who they are.
	void ClientUserInfoChanged(CBasePlayer* pPlayer, char* infobuffer) override;

	// Half-Life would announce "* Red changed name to Dave" to everyone the
	// moment the engine put a player's real name back on them.
	bool ShouldAnnounceNameChange() override { return !m_bAnonActive; }

	// Both ends of an occupancy wipe the slot - see ResetPlayerSlot().
	bool ClientConnected(edict_t* pEntity, const char* pszName, const char* pszAddress, char szRejectReason[128]) override;
	void ClientDisconnected(edict_t* pClient) override;

	// GetPlayerByIndex() for the "ch_odds" server command, which is a free
	// function and so cannot reach the private one.
	static CBasePlayer* GetPlayerByIndexPublic(int index) { return GetPlayerByIndex(index); }

	// --- Killer draw odds (read-only; for the admin display) ---
	// A player's current share of the Killer draw, in percent. Returns 0 for a
	// slot that is not in the draw at all (empty, or connected but dead).
	float GetKillerChancePercent(int index) const;

	// The raw weight behind that share, 1.0 being "never been the Killer
	// recently". Kept separate because the percentage moves when *anyone*
	// joins or dies, and a display that wants to show why wants both.
	float GetKillerWeight(int index) const;

	bool        IsMultiplayer() override { return true; }
	bool        IsDeathmatch() override { return true; }
	bool        IsCoOp() override { return false; }

private:
	// --- round flow ---
	void SetRoundState(CHRoundState state);
	void StartPreRound();
	void StartRound();
	void EndRound(CHRole winningRole);
	void AnnounceWaitingForPlayers() const;
	void AbortRound();        // too few players left - drop the round on the spot
	void ResetForNextRound(); // reset roles, go back to WaitingForPlayers/PreRound

	// --- movement ---
	// PM_CheckParamters() only ever clamps a player *down* to pev->maxspeed, so
	// sv_maxspeed has to sit at the fastest speed anyone in the mode may reach
	// and every player then gets held below it individually.
	static void EnforceSpeedCeiling();
	void        UpdatePlayerSpeed(CBasePlayer* pPlayer) const;

	// --- misfire punishment ---
	// Start the penalty running on a player who shot an innocent.
	void PunishShooter(CBasePlayer* pPlayer);

	// The whole penalty - dropped revolver, speed and jump - lasts exactly
	// ch_punish_time seconds, so this one answer drives all three.
	bool IsPunished(CBasePlayer* pPlayer) const;

	// Per-frame upkeep: expire the timer, take the revolver away, and keep the
	// client's predicted jump height in step with the penalty.
	void        ServicePunishment(CBasePlayer* pPlayer);
	void        ClearPunishments();
	static void UpdatePlayerJump(CBasePlayer* pPlayer, bool bPunished);

	// Darken or clear one player's view. The tint holds until it is told
	// otherwise, so every path that ends a penalty has to turn it off.
	static void SetPunishTint(CBasePlayer* pPlayer, bool bOn);

	// --- map reset ---
	void        TakeMapSnapshot();  // record spawn state of resettable entities (once per map)
	void        ResetMapEntities(); // put the world back the way the map loaded
	static void RemoveRoundLitter(); // gibs, corpsebags, live ordnance, map guns

	// --- per-slot state ---
	// Forget everything this mode knows about one client slot. Every array
	// below indexed by ENTINDEX() must be cleared here and nowhere else - see
	// the comment on the implementation for why that matters.
	void ResetPlayerSlot(int index);

	// --- Killer draw weighting ---
	// Draw the Killer in proportion to m_flKillerWeight, so a player who was
	// just the Killer is unlikely to be handed it again straight away.
	CBasePlayer* PickWeightedKiller() const;

	// Fold this round's draw back into the weights: the player who was picked
	// decays, everyone else who was eligible recovers. Only ever called once
	// per draw, from AssignRoles().
	void AgeKillerWeights(CBasePlayer* pKiller);

	// Is this slot in the Killer draw right now? The one definition of
	// eligibility, shared by the draw, the ageing and the odds display.
	bool IsKillerCandidate(int index) const;

	// Total weight across everyone eligible, 0 if nobody is.
	float TotalKillerWeight() const;

	// --- role management ---
	void   AssignRoles();
	void   GiveRoleLoadout(CBasePlayer* pPlayer, CHRole role);
	void   SetPlayerRole(CBasePlayer* pPlayer, CHRole role);
	CHRole GetPlayerRole(CBasePlayer* pPlayer) const;
	void   AnnounceRole(CBasePlayer* pPlayer, CHRole role) const;

	// Is a weapon classname one this role is allowed to carry?
	static bool RoleCanCarryWeapon(CHRole role, const char* pszWeaponName);

	// --- anonymous mode ---
	// Deal every connected player a fresh colour and name for the round, or
	// take the disguises off if the cvar has since been turned off. Called from
	// AssignRoles(), so identities re-roll on exactly the round boundary roles
	// do - a colour that survived into the next round would be a name.
	void AssignAnonIdentities();

	// Give one slot an identity and push it out over their userinfo.
	void DealAnonIdentity(CBasePlayer* pPlayer, int colorIndex, const char* pszName);

	// Re-stamp a slot's dealt identity over whatever the client just sent.
	void ApplyAnonIdentity(CBasePlayer* pPlayer);

	// Put everyone's own name and colours back.
	void ClearAnonIdentities();

	// Remember what a player really looks like, once, so ClearAnonIdentities()
	// has something to put back. Reading it any later than the first userinfo
	// we see would just read our own disguise back.
	void StashRealIdentity(CBasePlayer* pPlayer, const char* pszInfoBuffer);

	// Tell clients which colour to draw each player's name in. A null pTo
	// broadcasts to everyone.
	void SendAnonColors(CBasePlayer* pTo) const;

	// Fill the name pool from the names file, falling back to the built-in
	// list if it is missing or empty.
	static void LoadAnonNames();

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

	// Park an alive, roleless player in a live round (a mid-round joiner) in
	// observer mode. Deferred out of PlayerSpawn(), which runs too early for
	// the spectator view to survive ClientPutInServer().
	void EnforceObserverForUnassigned(CBasePlayer* pPlayer) const;

	// Send the scoreboard title (the "hostname" cvar). Deferred out of
	// PlayerSpawn() the same way: the base class sends it once from InitHUD(),
	// which on a listen server runs before the client has hooked user messages
	// at all, so that copy is dropped by the engine and never asked for again.
	void ServiceServerNameSend(CBasePlayer* pPlayer);

	// Leave a visible body behind at the point a player was killed.
	static void LeaveCorpse(CBasePlayer* pPlayer);

	void CheckRoundWinConditions();

	CHRoundState m_roundState;
	float        m_flStateEnterTime; // gpGlobals->time when we entered m_roundState

	// how long each phase lasts, in seconds - tune to taste / expose as cvars
	float m_flPreRoundLength;
	float m_flRoundEndLength;

	// gpGlobals->time of the next "Waiting for players..." announcement
	float m_flNextWaitingAnnounce;

	// role assigned to each possible player slot, indexed by ENTINDEX() (1..MAX_PLAYERS)
	CHRole m_playerRoles[MAX_PLAYERS + 1];

	// Each slot's share of the Killer draw, relative to the others. 1.0 is the
	// baseline everyone starts at and recovers back to; being drawn as the
	// Killer knocks it down. Cleared with the slot, so a player who reconnects
	// comes back at full odds - see ResetPlayerSlot().
	float m_flKillerWeight[MAX_PLAYERS + 1];

	// gpGlobals->time to send each player the scoreboard title, or 0 for nothing
	// pending. Same indexing as m_playerRoles.
	float m_flSendServerName[MAX_PLAYERS + 1];

	// gpGlobals->time each punished player's penalty runs out, or 0 for no
	// penalty. Same indexing as m_playerRoles, and cleared with it: the penalty
	// is a cost paid inside one round, not something carried into the next.
	float m_flPunishEndTime[MAX_PLAYERS + 1];

	// Anonymous mode state, all indexed like m_playerRoles.
	//
	// m_bAnonActive is what everything else keys off: identities are applied
	// right now. It is not the same as the cvar being on - the cvar can be
	// flipped mid-round, and the disguises only come off at the next deal.
	bool m_bAnonActive;
	int  m_anonColor[MAX_PLAYERS + 1]; // index into g_CHAnonColors, -1 for none
	char m_szAnonName[MAX_PLAYERS + 1][CH_MAX_ANON_NAME];

	// What the player actually looks like, kept so it can be given back.
	// m_realTopColor < 0 means nothing has been stashed for this slot yet.
	char m_szRealName[MAX_PLAYERS + 1][CH_MAX_ANON_NAME];
	int  m_realTopColor[MAX_PLAYERS + 1];
	int  m_realBottomColor[MAX_PLAYERS + 1];

	// Spawn-time state of every resettable map entity, taken once on the first
	// frame after the map has finished spawning its entities.
	CHEntitySnapshot m_mapSnapshot[CH_MAX_TRACKED_ENTITIES];
	int              m_numSnapshots;
	bool             m_bSnapshotTaken;
};
