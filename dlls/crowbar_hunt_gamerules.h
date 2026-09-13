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
#include "crowbar_hunt_loot.h"

// Role a player currently holds for the round
enum class CHRole
{
	Unassigned = 0,

	// Asked to sit out with the "spectate" command, and stays out until they ask
	// to come back. Distinct from Unassigned, which is temporary - a player who
	// missed a deal and is owed a place in the next one. Every head count, role
	// deal and forced respawn skips this role. See HandleSpectateCommand().
	Spectator,

	Killer,
	Hunter,
	Survivor,
};

// How often, in seconds, WaitingForPlayers re-sends its on-screen notice.
// The notice's hold time is a little longer than this, so re-sending on the
// same channel keeps it up continuously (and the player count current) until
// the state changes.
#define CH_WAITING_ANNOUNCE_INTERVAL 1.0f

// How long, in seconds, after the round is decided before the result goes up
// on screen - a beat for the last kill to land before the verdict.
#define CH_ROUND_OVER_DELAY 2.0f

// Played to everyone as the result appears. Placeholder until one is chosen.
#define CH_ROUND_OVER_SOUND "buttons/elevbell1.wav"

// How long, in seconds, the "really spectate?" prompt stays up and its answer
// is accepted.
#define CH_SPECTATE_PROMPT_TIME 10

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

class CCrowbarHuntCorpse;

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

	// Crowbar swings, body hits, weapon pickups, death cries and the death
	// alarm all give away the Killer (or that someone just died) from out of
	// sight. Silence them for everyone; wall hits stay audible.
	bool PlayGiveawaySounds() override { return false; }

	// Armour is not part of the game - there is no loadout that has it and the
	// map's batteries are removed - so the wall chargers are spawned drained
	// and never refill. They still hum and deny like a used-up one would.
	bool AllowHEVChargers() override { return false; }
	float FlHEVChargerRechargeTime() override { return 0; }

	// A player who asks to spectate stays a spectator through the round resets
	// that would otherwise respawn them, until they ask to come back. Leaving a
	// live round asks for confirmation first (a ShowMenu prompt); the answer
	// comes back through ClientCommand() as "menuselect".
	bool HandleSpectateCommand(CBasePlayer* pPlayer) override;
	bool ClientCommand(CBasePlayer* pPlayer, const char* pcmd) override;

	// Voice rules for a live round. The dead never reach the living (dying
	// must not be a way to name the Killer), and with ch_proxvoice on the
	// living only hear each other nearby: hiding is the Survivors' main
	// defence, so a voice that carries the whole map would give them away.
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

	// "- X has left the game" names the player who joined, not the identity
	// they were wearing: the round's name is meaningless once they are gone,
	// and a room-wide "Alpha left" leaks nothing a scoreboard glance did not.
	const char* GetClientLeaveName(edict_t* pClient) override;

	// The name a player joined with, or "" for an empty slot. This is what
	// the userinfo name key stops answering once a round has dealt colours
	// and names, and what an admin means when they type a name.
	const char* GetRealName(int index) const;

	// Both ends of an occupancy wipe the slot - see ResetPlayerSlot().
	bool ClientConnected(edict_t* pEntity, const char* pszName, const char* pszAddress, char szRejectReason[128]) override;
	void ClientDisconnected(edict_t* pClient) override;

	// Tells the client's scoreboard to use the Players/Spectators layout. The
	// base class sends this once from InitHUD(), which a listen server's client
	// drops (see ServiceServerNameSend()), so it is also re-sent from there.
	void UpdateGameMode(CBasePlayer* pPlayer) override;

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

	// Whether this slot may open the admin odds panel: the listen server host,
	// or anyone who has given ch_admin_pass this connection. Never bots.
	bool IsAdmin(int index) const;

	// A player walked into a piece of loot. Returns false to leave it lying
	// there: outside a live round, or for anyone not playing this round.
	bool CollectLoot(CBasePlayer* pPlayer);

	// Fill every empty loot spot at once, ignoring the interval and the cap.
	// For the "ch_loot_respawn" server command. Returns how many were placed.
	int SpawnAllLoot();

	// Dump the loot table to the server console, for "ch_loot_list".
	void PrintLootTable();

	// A player +used a corpse. If they are the Killer and can pay, they walk
	// away wearing the dead player's name and colours - see the disguise
	// section in the .cpp. Returns whether a disguise was put on.
	bool TryDisguise(CBasePlayer* pPlayer, CCrowbarHuntCorpse* pCorpse);

	bool        IsMultiplayer() override { return true; }
	bool        IsDeathmatch() override { return true; }
	bool        IsCoOp() override { return false; }

private:
	// --- round flow ---
	void SetRoundState(CHRoundState state);
	void StartPreRound();
	void StartRound();
	void EndRound(CHRole winningRole, const char* pszMessage = nullptr);
	void AnnounceWaitingForPlayers() const;
	void ClearWaitingForPlayers() const;
	void AnnounceRoundOver();
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

	// --- loot ---
	// Once per map, after its entities have spawned: if the author placed
	// ch_loot_spawn markers, they replace whatever the loot file gave us.
	void ResolveLootSpawns();

	// Every ch_loot_interval seconds of a live round, put a piece of loot on
	// a random empty spot, up to ch_loot_max lying around at once.
	void ServiceLootSpawns();

	// Push one player's count to their HUD.
	void SendLootCount(CBasePlayer* pPlayer) const;

	// Zero every count and tell everyone so - the start of a deal.
	void ClearLootCounts();

	// A player just crossed a ch_loot_reward threshold: arm them, or if they
	// are armed (or barred from arming) put the gun on the floor beside them.
	void AwardLootRevolver(CBasePlayer* pPlayer);

	// --- map reset ---
	void        TakeMapSnapshot();  // record spawn state of resettable entities (once per map)
	void        ResetMapEntities(); // put the world back the way the map loaded
	static void RemoveRoundLitter(); // gibs, corpsebags, live ordnance, map guns/ammo/batteries/longjumps/medkits

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

	// Push the whole odds table to one client for its admin panel: a CHAdmin
	// message per connected player, then the terminator. See UserMessages.h.
	void SendKillerOdds(CBasePlayer* pPlayer) const;

	// --- role management ---
	void   AssignRoles();
	void   GiveRoleLoadout(CBasePlayer* pPlayer, CHRole role);
	void   SetPlayerRole(CBasePlayer* pPlayer, CHRole role);
	CHRole GetPlayerRole(CBasePlayer* pPlayer) const;
	void   AnnounceRole(CBasePlayer* pPlayer, CHRole role) const;

	// Is a weapon classname one this role is allowed to carry?
	static bool RoleCanCarryWeapon(CHRole role, const char* pszWeaponName);

	// Has this slot asked to sit out? The one definition of "leave them alone",
	// shared by the head count, the role deal and both respawn sweeps.
	bool IsSittingOut(int index) const;
	void BecomeSpectator(CBasePlayer* pPlayer);

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

	// Put one slot's own name and colours back, if they were ever stashed.
	void RestoreRealIdentity(int index);

	// Stamp whatever a slot should currently be seen as over their userinfo:
	// a disguise if they are wearing one, else the round's anonymous identity
	// if one is active, else nothing. The one entry point for "re-apply".
	void ApplyIdentity(CBasePlayer* pPlayer);

	// --- disguise ---
	// Take a disguise off one player, or off everyone. Both fall back to the
	// identity underneath (anonymous or real) and tell clients.
	void ClearDisguise(CBasePlayer* pPlayer, bool bTell);
	void ClearAllDisguises();

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

	// Park a player who is not taking part in observer mode: an alive, roleless
	// player in a live round (a mid-round joiner), or anyone who asked to sit
	// out. Deferred out of PlayerSpawn(), which runs too early for the spectator
	// view to survive ClientPutInServer().
	void EnforceObserverForSidelined(CBasePlayer* pPlayer) const;

	// Send the scoreboard title (the "hostname" cvar). Deferred out of
	// PlayerSpawn() the same way: the base class sends it once from InitHUD(),
	// which on a listen server runs before the client has hooked user messages
	// at all, so that copy is dropped by the engine and never asked for again.
	void ServiceServerNameSend(CBasePlayer* pPlayer);

	// Broadcast whether a player is sitting out (CHRole::Spectator) whenever
	// that changes, so every client's scoreboard can file them under
	// Spectators. Only voluntary spectators count: the round's dead stay under
	// Players. The client cannot work any of this out on its own.
	void ServiceSpectatorState(CBasePlayer* pPlayer);

	// Tell a client whether distance falloff applies to the voices it hears,
	// whenever that changes. The client cannot know ch_proxvoice or the round
	// state on its own; it does the per-frame fading itself.
	void ServiceProxVoiceState(CBasePlayer* pPlayer);
	static void SendSpectatorState(CBasePlayer* pPlayer, bool bObserver, edict_t* pTarget);

	// Leave a visible body behind at the point a player was killed.
	void LeaveCorpse(CBasePlayer* pPlayer) const;

	void CheckRoundWinConditions();

	CHRoundState m_roundState;
	float        m_flStateEnterTime; // gpGlobals->time when we entered m_roundState

	// how long each phase lasts, in seconds - tune to taste / expose as cvars
	float m_flPreRoundLength;
	float m_flRoundEndLength;

	// gpGlobals->time of the next "Waiting for players..." announcement
	float m_flNextWaitingAnnounce;

	// The round's result, held back by CH_ROUND_OVER_DELAY. The time it goes
	// up, or 0 when there is nothing pending; the text is whatever EndRound()
	// was given. See AnnounceRoundOver().
	float m_flRoundOverAnnounceTime;
	char  m_szRoundOverMessage[128];

	// gpGlobals->time the live round is called for the Survivors, from
	// ch_round_time. 0 while no clock is running - outside InProgress, or when
	// the cvar is 0.
	float m_flRoundTimeLimit;

	// Push the round clock to one client, or to everyone when pTarget is null:
	// the seconds left, or 0 to take it off the HUD.
	void SendRoundTimer(edict_t* pTarget) const;
	void SendRoleHud(CBasePlayer* pPlayer) const;

	// role assigned to each possible player slot, indexed by ENTINDEX() (1..MAX_PLAYERS)
	CHRole m_playerRoles[MAX_PLAYERS + 1];

	// Each slot's share of the Killer draw, relative to the others. 1.0 is the
	// baseline everyone starts at and recovers back to; being drawn as the
	// Killer knocks it down. Cleared with the slot, so a player who reconnects
	// comes back at full odds - see ResetPlayerSlot().
	float m_flKillerWeight[MAX_PLAYERS + 1];

	// Whether each slot has proven itself an admin - connected from the listen
	// server's own machine, or typed ch_admin_pass once. Cleared with the slot.
	bool m_bAdmin[MAX_PLAYERS + 1];

	// gpGlobals->time to send each player the scoreboard title, or 0 for nothing
	// pending. Same indexing as m_playerRoles.
	float m_flSendServerName[MAX_PLAYERS + 1];

	// The sitting-out state last broadcast for each slot: 0, 1, or -1 for
	// "never sent", which forces a send on a slot's first PlayerThink(). Same indexing
	// as m_playerRoles, and cleared with it.
	int m_iSentSpectator[MAX_PLAYERS + 1];

	// The proximity-falloff state last sent to each slot, same scheme as
	// m_iSentSpectator: 0, 1, or -1 for "never sent".
	int m_iSentProxVoice[MAX_PLAYERS + 1];

	// gpGlobals->time each punished player's penalty runs out, or 0 for no
	// penalty. Same indexing as m_playerRoles, and cleared with it: the penalty
	// is a cost paid inside one round, not something carried into the next.
	float m_flPunishEndTime[MAX_PLAYERS + 1];

	// gpGlobals->time until which a "menuselect 1" from this slot counts as a
	// yes to the spectate prompt, or 0 for no prompt up. Matches the menu's own
	// on-screen timeout, so a stale answer to some later menu cannot be taken
	// as consent. Same indexing as m_playerRoles, and cleared with it.
	float m_flSpectatePromptExpires[MAX_PLAYERS + 1];

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

	// Disguise state, indexed like m_playerRoles and cleared with it. Only the
	// Killer ever wears one, but it is kept per slot so the identity code does
	// not have to know who the Killer is. It sits *over* the anonymous identity
	// rather than replacing it, so taking it off puts back whatever was dealt.
	bool m_bDisguised[MAX_PLAYERS + 1];
	char m_szDisguiseName[MAX_PLAYERS + 1][CH_MAX_ANON_NAME];
	int  m_iDisguiseTopColor[MAX_PLAYERS + 1];
	int  m_iDisguiseBottomColor[MAX_PLAYERS + 1];
	int  m_iDisguiseAnonColor[MAX_PLAYERS + 1]; // the name colour clients draw, -1 for none

	// Spawn-time state of every resettable map entity, taken once on the first
	// frame after the map has finished spawning its entities.
	CHEntitySnapshot m_mapSnapshot[CH_MAX_TRACKED_ENTITIES];
	int              m_numSnapshots;
	bool             m_bSnapshotTaken;

	// Where loot may appear on this map. Filled from the loot file by the
	// constructor, then replaced by the map's own markers if it has any - see
	// ResolveLootSpawns().
	CHLootSpawn m_lootSpawns[CH_MAX_LOOT_SPAWNS];
	int         m_numLootSpawns;
	bool        m_bLootSpawnsResolved;

	// gpGlobals->time of the next loot spawn while a round is live.
	float m_flNextLootSpawn;

	// Loot collected this round, indexed like m_playerRoles and cleared with it.
	int m_iLootCount[MAX_PLAYERS + 1];

	// Revolvers bought with it this round, same indexing. The next one comes at
	// (this + 1) * ch_loot_reward pieces; for the Killer, one at twice the price.
	int m_iLootRewards[MAX_PLAYERS + 1];

	// Set only for the moment AwardLootRevolver() is handing the Killer their
	// bought gun, so CanHavePlayerItem() lets that one through and no other -
	// the Hunter's dropped revolver stays off-limits to them either way.
	bool m_bGrantingKillerGun;
};

// The running Crowbar Hunt rules, or null if some other mode is installed.
// For entities that only exist under this mode and need to talk back to it.
CHalfLifeCrowbarHunt* CH_GetCrowbarHuntRules();

// Which kind of world-lying pickup this is, for CH_SetWeaponGlow().
enum class CHWeaponGlow
{
	Crowbar, // red
	Revolver, // blue
	Loot, // green
};

// Put a coloured glow shell on a pickup that is lying in (or flying through)
// the world, so players can tell it from the scenery. Safe to call on any
// entity; does nothing when ch_glow_shell is 0.
void CH_SetWeaponGlow(CBaseEntity* pEntity, CHWeaponGlow weapon);
