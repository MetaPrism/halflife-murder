#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "player.h"
#include "weapons.h"
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
// There are two behaviours, and two cvars choose between three outcomes.
// bot_zombie is the master: while it is set nothing moves at all, which is the
// original behaviour and what a bot that only has to fill a player slot wants.
// With bot_zombie off, bot_hunt picks the AI - 0 is the aimless wander this
// file started as, 1 is the hunting bot further down, which reads the world
// with hull traces, gets over what is in its way, and swings at whoever it
// walks into.
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

// When a bot stops making progress, it first tries a crouch jump in place -
// that clears the low ledges and stair lips a flat wander can't - before
// giving up on the yaw and picking a new one.
static bool g_bBotJumping[MAX_PLAYERS + 1] = {};

// How often a bot's progress is sampled, and how far it has to have travelled
// in that time to count as still moving.
constexpr float BOT_PROGRESS_INTERVAL = 0.3f;
constexpr float BOT_PROGRESS_DISTANCE = 16.0f;
constexpr float BOT_JUMP_ATTEMPT_DURATION = 0.5f;

static void BotPickNewDirection(int index, CBaseEntity* pPlayer)
{
	g_flBotYaw[index] = RANDOM_FLOAT(-180, 180);
	g_vecBotLastOrigin[index] = pPlayer->pev->origin;
	g_flBotNextProgressCheck[index] = gpGlobals->time + BOT_PROGRESS_INTERVAL;
}

// ---------------------------------------------------------------------------
// Hunting bots: bot_hunt 1
// ---------------------------------------------------------------------------

// Everything below is one behaviour, and it decides what to do from hull traces
// and from whether the bot actually moved, and from nothing else - no node
// graph, no navmesh, no knowledge of the map. That is what lets it be dropped
// on any map in the rotation and still get somewhere.
//
// The loop, in order:
//
//   walk the current heading until the way ahead is shut;
//   if crouching opens it, crouch and carry on;
//   otherwise crouch-jump forward at full height, and keep the heading if the
//     landing got somewhere;
//   otherwise turn at least BOT_HUNT_TURN_MIN degrees off the heading and
//     start again.
//
// On top of that, a bot touching another player draws the crowbar and swings,
// and a bot whose crosshair happens to cross another player draws whatever gun
// it has and fires. It does not aim: it notices.

// Whether a bot puts its weapon away once it is done with it.
//
//   MURDER 1: it does, by selecting weapon_hands - this mod's holster, the
//     weapon that draws nothing - so a Killer bot goes back to looking like a
//     Survivor between kills, and a Hunter bot keeps the revolver a secret
//     until the next time it needs it. This is the default because concealment
//     is the whole of the mode.
//   MURDER 0: it does not, and walks around with the crowbar out.
#define MURDER 1

// Named rather than counted, because Crowbar Hunt puts weapon_hands and
// weapon_crowbar in the same HUD slot - iItemSlot() is 1 for both - and a slot
// number cannot tell the holster from the murder weapon.
#define BOT_HUNT_MELEE_WEAPON "weapon_crowbar"
#define BOT_HUNT_STOW_WEAPON "weapon_hands"

// The slot a gun is looked for in. Every real weapon in this mode lives here
// (weapons.h: the glock and the python are both iItemSlot() 2), and what is
// found is only fired if it really is ranged, so nothing here has to know which
// gun a role was given.
constexpr int BOT_HUNT_GUN_SLOT = 2;

// How far ahead the way is checked, and the 18 units the engine walks a player
// up without being asked: a hull traced along the floor calls every staircase a
// wall, so the trace is retried from the top of a step before the way is
// believed shut.
constexpr float BOT_HUNT_LOOKAHEAD = 24.0f;
constexpr float BOT_HUNT_STEP_HEIGHT = 18.0f;

// Half the height of each hull, measured up from the feet, and the hull numbers
// to trace them with.
constexpr float BOT_HUNT_STAND_CENTRE = 36.0f;
constexpr float BOT_HUNT_DUCK_CENTRE = 18.0f;

// A jump counts as having got somewhere if it ended this much higher than it
// started, or this much further along the heading. A jump that has not landed
// by the timeout is a fall - or a bot somewhere a jump does not work, on a
// ladder or in water - and is given up on.
constexpr float BOT_HUNT_JUMP_GAIN_Z = 8.0f;
constexpr float BOT_HUNT_JUMP_GAIN_FORWARD = 16.0f;
constexpr float BOT_HUNT_JUMP_TIMEOUT = 2.0f;

// The smallest turn taken when a heading is given up on. Anything less and the
// bot walks back into the same wall from very nearly the same angle.
constexpr float BOT_HUNT_TURN_MIN = 45.0f;

// How close another player has to be to be worth swinging at, measured between
// origins: a shade more than two player hulls, so the bot starts swinging as it
// bumps rather than after. Crowbar reach does the rest.
constexpr float BOT_HUNT_MELEE_RANGE = 48.0f;
constexpr float BOT_HUNT_MELEE_HEIGHT = 72.0f;

// How far a bot can see a target down its crosshair.
constexpr float BOT_HUNT_SIGHT_RANGE = 2048.0f;

// Drawing a weapon takes the weapon's own deploy time, and a button pressed
// before that has run out is thrown away. Comfortably longer than the half
// second DefaultDeploy() asks for.
constexpr float BOT_HUNT_DRAW_TIME = 0.6f;

// How long the attack button is held for one swing, and how long it is let go
// of afterwards. The crowbar will keep swinging on a held button, but letting
// go keeps a swing a swing and gives the bot a frame to notice its victim left.
constexpr float BOT_HUNT_SWING_TIME = 0.3f;
constexpr float BOT_HUNT_RECOVER_TIME = 0.2f;

// A crosshair leaves a moving target for a frame at a time without the bot
// having lost it, and a weapon switch that did not take is waited out rather
// than retried every frame for the rest of the map.
constexpr float BOT_HUNT_TARGET_GRACE = 0.4f;
constexpr float BOT_HUNT_DRAW_RETRY = 0.5f;

enum class BotHuntMove
{
	Wander, // walking, ducking under things, turning
	Jump,	// committed to a crouch jump and seeing where it lands
};

enum class BotHuntMelee
{
	Idle,	 // nothing in reach, nothing drawn on its account
	Draw,	 // switched to the crowbar, waiting out the deploy
	Swing,	 // attack held
	Recover, // attack released, deciding whether to swing again
};

enum class BotHuntGun
{
	Idle,
	Draw,
	Fire,
};

// The hunting bot's state. The heading, the last sampled origin and the next
// sample time are the wander's own arrays above: both behaviours want exactly
// those three things and mean the same by them, so switching bot_hunt mid-round
// leaves a bot walking the way it already was.
struct bothunt_t
{
	BotHuntMove move;
	Vector vecJumpStart;
	bool bAirborne; // left the floor, so it is time to tuck the legs up
	float flJumpExpire;

	BotHuntMelee melee;
	float flMeleeNext;

	BotHuntGun gun;
	float flGunNext;
	float flTargetLost; // 0 while the crosshair is still on somebody
};

static bothunt_t g_botHunt[MAX_PLAYERS + 1] = {};

// The move being assembled for one bot this frame. Combat takes the view,
// movement takes the legs, and the engine is handed the result once at the end.
struct bothuntmove_t
{
	float flYaw;
	float flForward;
	int buttons;
};

static void BotHuntReset(int index)
{
	g_botHunt[index] = bothunt_t();
}

static float BotHuntSpeed(CBasePlayer* pPlayer)
{
	// Anything asked for above the player's own maximum is clipped by the
	// movement code; this only avoids asking for zero.
	return pPlayer->pev->maxspeed > 0 ? pPlayer->pev->maxspeed : 240.0f;
}

static void BotHuntSampleProgress(int index, CBasePlayer* pPlayer)
{
	g_vecBotLastOrigin[index] = pPlayer->pev->origin;
	g_flBotNextProgressCheck[index] = gpGlobals->time + BOT_PROGRESS_INTERVAL;
}

// Gives up on the current heading for one at least BOT_HUNT_TURN_MIN degrees
// off it, to either side.
static void BotHuntTurnAway(int index, CBasePlayer* pPlayer)
{
	const float flTurn = RANDOM_FLOAT(BOT_HUNT_TURN_MIN, 180.0f);

	g_flBotYaw[index] = UTIL_AngleMod(g_flBotYaw[index] + (RANDOM_LONG(0, 1) ? flTurn : -flTurn));
	BotHuntSampleProgress(index, pPlayer);
}

// Is there room to walk BOT_HUNT_LOOKAHEAD units along vecForward, standing or
// ducked? Traced as a hull rather than a line, because what matters is whether
// the player fits - and the whole point of the ducked answer is that the
// standing one does not.
static bool BotHuntPathClear(CBasePlayer* pPlayer, const Vector& vecForward, bool bDuck)
{
	Vector vecFeet = pPlayer->pev->origin;

	vecFeet.z += pPlayer->pev->mins.z; // the bottom of whichever hull it has now

	const int hull = bDuck ? head_hull : human_hull;
	const float flCentre = bDuck ? BOT_HUNT_DUCK_CENTRE : BOT_HUNT_STAND_CENTRE;

	// Two traces, and the second is the interesting one. Flat along the floor
	// first: if that is clear the bot can simply walk. If it is not, the same
	// trace from the top of a step answers the different question of whether
	// the thing in the way is something the engine will walk the bot up - a
	// stair lip, a kerb, a doorway's threshold. Raising the hull is safe here
	// precisely because it is only asked once the flat trace has failed, so a
	// low ceiling never reaches this trace on its own.
	for (int pass = 0; pass < 2; ++pass)
	{
		Vector vecStart = vecFeet;

		vecStart.z += flCentre + (pass ? BOT_HUNT_STEP_HEIGHT : 0.0f);

		TraceResult tr;
		UTIL_TraceHull(vecStart, vecStart + vecForward * BOT_HUNT_LOOKAHEAD, dont_ignore_monsters, hull, pPlayer->edict(), &tr);

		if (!tr.fAllSolid && !tr.fStartSolid && tr.flFraction == 1.0f)
			return true;
	}

	return false;
}

// A weapon is ranged if it has an ammo type, which is how the crowbar and the
// hands answer the question of whether they are guns: pszAmmo1() is NULL for
// both. The first thing in the gun slot with something left to fire wins.
static CBasePlayerItem* BotHuntGunInSlot(CBasePlayer* pPlayer)
{
	for (CBasePlayerItem* pItem = pPlayer->m_rgpPlayerItems[BOT_HUNT_GUN_SLOT]; pItem; pItem = pItem->m_pNext)
	{
		const char* pszAmmo = pItem->pszAmmo1();

		if (!pszAmmo)
			continue;

		const int iAmmo = CBasePlayer::GetAmmoIndex(pszAmmo);

		if (iAmmo < 0 || iAmmo >= MAX_AMMO_SLOTS)
			continue;

		// A clip weapon with rounds in it can fire with no reserve left, and a
		// weapon with no clip at all reports -1 and lives off the reserve.
		CBasePlayerWeapon* pWeapon = static_cast<CBasePlayerWeapon*>(pItem);

		if (pWeapon->m_iClip > 0 || pPlayer->m_rgAmmo[iAmmo] > 0)
			return pItem;
	}

	return nullptr;
}

// Puts away whatever is out, if this build hides its weapons.
static void BotHuntStow(CBasePlayer* pPlayer)
{
	if (0 == MURDER)
		return;

	if (pPlayer->HasNamedPlayerItem(BOT_HUNT_STOW_WEAPON))
		pPlayer->SelectItem(BOT_HUNT_STOW_WEAPON);
}

// The nearest other living player close enough to call it a collision.
static CBasePlayer* BotHuntVictim(CBasePlayer* pSelf)
{
	CBasePlayer* pNearest = nullptr;
	float flNearest = BOT_HUNT_MELEE_RANGE;

	for (int i = 1; i <= gpGlobals->maxClients && i <= MAX_PLAYERS; ++i)
	{
		CBaseEntity* pEntity = UTIL_PlayerByIndex(i);

		if (!pEntity || pEntity == pSelf || !pEntity->IsAlive())
			continue;

		const Vector vecDelta = pEntity->pev->origin - pSelf->pev->origin;

		if (fabs(vecDelta.z) > BOT_HUNT_MELEE_HEIGHT)
			continue;

		const float flDistance = vecDelta.Length2D();

		if (flDistance > flNearest)
			continue;

		flNearest = flDistance;
		pNearest = static_cast<CBasePlayer*>(pEntity);
	}

	return pNearest;
}

// Whoever the bot's crosshair is on, if it is on anybody. Traced from the gun
// straight down the view, which is all "the crosshair crossed them" means: the
// bot never turns to aim, so this only ever finds somebody it happened to walk
// past. Its view is level, so a target has to be at roughly its own height.
static CBasePlayer* BotHuntCrosshairPlayer(CBasePlayer* pSelf, const Vector& vecAngles)
{
	Vector vecForward, vecRight, vecUp;
	UTIL_MakeVectorsPrivate(vecAngles, vecForward, vecRight, vecUp);

	const Vector vecSrc = pSelf->GetGunPosition();

	TraceResult tr;
	UTIL_TraceLine(vecSrc, vecSrc + vecForward * BOT_HUNT_SIGHT_RANGE, dont_ignore_monsters, pSelf->edict(), &tr);

	if (FNullEnt(tr.pHit))
		return nullptr;

	CBaseEntity* pHit = CBaseEntity::Instance(tr.pHit);

	if (!pHit || !pHit->IsPlayer() || !pHit->IsAlive())
		return nullptr;

	return static_cast<CBasePlayer*>(pHit);
}

// Returns true while a swing is in progress, which tells the rest of the frame
// that the view belongs to the fight and that a bot standing still to hit
// somebody has not stopped making progress.
static bool BotHuntMeleeThink(int index, CBasePlayer* pPlayer, bothuntmove_t& move)
{
	bothunt_t* pHunt = &g_botHunt[index];
	CBasePlayer* pVictim = BotHuntVictim(pPlayer);

	if (BotHuntMelee::Idle == pHunt->melee)
	{
		if (!pVictim || gpGlobals->time < pHunt->flMeleeNext || !pPlayer->HasNamedPlayerItem(BOT_HUNT_MELEE_WEAPON))
			return false;

		// Nothing is swung this frame: Deploy() sets the weapon's own timers
		// and an attack pressed before they run out is thrown away.
		pPlayer->SelectItem(BOT_HUNT_MELEE_WEAPON);

		pHunt->melee = BotHuntMelee::Draw;
		pHunt->flMeleeNext = gpGlobals->time + BOT_HUNT_DRAW_TIME;
	}

	// Face what is being hit. A swing is a short trace straight out of the
	// bot's own view, so a bot that keeps its wandering heading misses from
	// touching distance. The heading itself is left alone: when the fight is
	// over the bot carries on the way it was going.
	if (pVictim)
		move.flYaw = UTIL_VecToYaw(pVictim->pev->origin - pPlayer->pev->origin);

	switch (pHunt->melee)
	{
	case BotHuntMelee::Draw:
		if (gpGlobals->time >= pHunt->flMeleeNext)
		{
			pHunt->melee = BotHuntMelee::Swing;
			pHunt->flMeleeNext = gpGlobals->time + BOT_HUNT_SWING_TIME;
		}
		break;

	case BotHuntMelee::Swing:
		move.buttons |= IN_ATTACK;

		if (gpGlobals->time >= pHunt->flMeleeNext)
		{
			pHunt->melee = BotHuntMelee::Recover;
			pHunt->flMeleeNext = gpGlobals->time + BOT_HUNT_RECOVER_TIME;
		}
		break;

	case BotHuntMelee::Recover:
		if (gpGlobals->time >= pHunt->flMeleeNext)
		{
			if (pVictim)
			{
				pHunt->melee = BotHuntMelee::Swing;
				pHunt->flMeleeNext = gpGlobals->time + BOT_HUNT_SWING_TIME;
			}
			else
			{
				// Nothing left to hit: hide it again, and do not draw it back
				// out on the very next frame if the victim is merely out of
				// reach rather than gone.
				BotHuntStow(pPlayer);

				pHunt->melee = BotHuntMelee::Idle;
				pHunt->flMeleeNext = gpGlobals->time + BOT_HUNT_DRAW_RETRY;
			}
		}
		break;

	default:
		break;
	}

	return BotHuntMelee::Idle != pHunt->melee;
}

static void BotHuntGunThink(int index, CBasePlayer* pPlayer, bothuntmove_t& move)
{
	bothunt_t* pHunt = &g_botHunt[index];
	CBasePlayerItem* pGun = BotHuntGunInSlot(pPlayer);
	CBasePlayer* pTarget = BotHuntCrosshairPlayer(pPlayer, Vector(0, move.flYaw, 0));

	switch (pHunt->gun)
	{
	case BotHuntGun::Idle:
		if (!pTarget || !pGun || gpGlobals->time < pHunt->flGunNext)
			break;

		if (pPlayer->m_pActiveItem == pGun)
		{
			// Already holding it, so there is nothing to wait for.
			pHunt->gun = BotHuntGun::Fire;
		}
		else
		{
			pPlayer->SelectItem(STRING(pGun->pev->classname));

			pHunt->gun = BotHuntGun::Draw;
			pHunt->flGunNext = gpGlobals->time + BOT_HUNT_DRAW_TIME;
		}

		pHunt->flTargetLost = 0;
		break;

	case BotHuntGun::Draw:
		if (gpGlobals->time < pHunt->flGunNext)
			break;

		if (pPlayer->m_pActiveItem != pGun)
		{
			// The switch did not take - something else is driving the
			// inventory. Wait before asking again.
			pHunt->gun = BotHuntGun::Idle;
			pHunt->flGunNext = gpGlobals->time + BOT_HUNT_DRAW_RETRY;
			break;
		}

		pHunt->gun = BotHuntGun::Fire;
		break;

	case BotHuntGun::Fire:
		if (!pGun || pPlayer->m_pActiveItem != pGun)
		{
			// Out of ammo, or something took the gun off it.
			pHunt->gun = BotHuntGun::Idle;
			pHunt->flGunNext = gpGlobals->time + BOT_HUNT_DRAW_RETRY;
			break;
		}

		if (pTarget)
		{
			move.buttons |= IN_ATTACK;
			pHunt->flTargetLost = 0;
			break;
		}

		if (0 == pHunt->flTargetLost)
		{
			pHunt->flTargetLost = gpGlobals->time;
		}
		else if (gpGlobals->time - pHunt->flTargetLost > BOT_HUNT_TARGET_GRACE)
		{
			// Lost them: put it away, so the revolver goes back to being a
			// secret until the next time the crosshair finds somebody.
			BotHuntStow(pPlayer);

			pHunt->gun = BotHuntGun::Idle;
			pHunt->flGunNext = gpGlobals->time + BOT_HUNT_DRAW_RETRY;
		}
		break;
	}
}

static void BotHuntJumpThink(int index, CBasePlayer* pPlayer, bothuntmove_t& move, const Vector& vecForward)
{
	bothunt_t* pHunt = &g_botHunt[index];
	const bool bOnGround = FBitSet(pPlayer->pev->flags, FL_ONGROUND) != 0;

	// Forward the whole way through: the jump is there to get somewhere, and a
	// crouch jump on the spot clears the obstacle and lands back in front of
	// it.
	move.flForward = BotHuntSpeed(pPlayer);

	// Checked before anything else, and on the ground as well as in the air. A
	// jump that has not landed by now is a long fall - but a bot that never
	// left the ground at all is somewhere a jump does not work, on a ladder or
	// in water or against a slope too steep to leave, and it would otherwise
	// hold the button down there for the rest of the map.
	if (gpGlobals->time >= pHunt->flJumpExpire)
	{
		pHunt->move = BotHuntMove::Wander;
		pHunt->bAirborne = false;
		BotHuntTurnAway(index, pPlayer);
		return;
	}

	if (!pHunt->bAirborne)
	{
		// Jump first and duck second, never both at once. That order is the
		// whole trick: the jump gets its full height from a standing player,
		// and ducking afterwards tucks the legs up for the last 18 units of
		// clearance. Ducking first gets neither.
		if (bOnGround)
			move.buttons |= IN_JUMP;
		else
			pHunt->bAirborne = true;

		return;
	}

	move.buttons |= IN_DUCK;

	if (!bOnGround)
		return;

	// Landed. It got somewhere if it came down higher than it went up, or
	// further along the heading than the wall it was standing against - either
	// way the obstacle is behind it now and the heading is worth keeping. If
	// not, the heading is what was wrong.
	const Vector vecTravel = pPlayer->pev->origin - pHunt->vecJumpStart;
	const bool bGained = vecTravel.z > BOT_HUNT_JUMP_GAIN_Z || DotProduct(vecTravel, vecForward) > BOT_HUNT_JUMP_GAIN_FORWARD;

	pHunt->move = BotHuntMove::Wander;
	pHunt->bAirborne = false;
	BotHuntSampleProgress(index, pPlayer);

	if (!bGained)
		BotHuntTurnAway(index, pPlayer);
}

static void BotHuntMoveThink(int index, CBasePlayer* pPlayer, bothuntmove_t& move, bool bFighting)
{
	bothunt_t* pHunt = &g_botHunt[index];

	Vector vecForward, vecRight, vecUp;
	UTIL_MakeVectorsPrivate(Vector(0, g_flBotYaw[index], 0), vecForward, vecRight, vecUp);

	if (BotHuntMove::Jump == pHunt->move)
	{
		BotHuntJumpThink(index, pPlayer, move, vecForward);
		return;
	}

	if (bFighting)
	{
		// Walk into whoever is being hit - move.flYaw is already pointed at
		// them - and read nothing into having stopped moving, which is exactly
		// what hitting somebody looks like from here.
		move.flForward = BotHuntSpeed(pPlayer);
		BotHuntSampleProgress(index, pPlayer);
		return;
	}

	// Two ways to find out the way ahead is shut. The trace sees a wall before
	// the bot is pressed against it; the progress sample catches everything the
	// trace is too coarse for - a corner taken at a shallow angle, a door
	// pushing back, another player in the way, a slope too steep to climb.
	bool bStuck = false;

	if (gpGlobals->time >= g_flBotNextProgressCheck[index])
	{
		bStuck = (pPlayer->pev->origin - g_vecBotLastOrigin[index]).Length2D() < BOT_PROGRESS_DISTANCE;
		BotHuntSampleProgress(index, pPlayer);
	}

	if (!bStuck && BotHuntPathClear(pPlayer, vecForward, false))
	{
		// Nothing in the way standing up. The standing trace answers the
		// headroom question too - its hull is the full 72 tall - so a bot under
		// a vent's ceiling finds it blocked and stays ducked.
	}
	else if (!bStuck && BotHuntPathClear(pPlayer, vecForward, true))
	{
		move.buttons |= IN_DUCK;
	}
	else if (FBitSet(pPlayer->pev->flags, FL_ONGROUND))
	{
		pHunt->move = BotHuntMove::Jump;
		pHunt->vecJumpStart = pPlayer->pev->origin;
		pHunt->bAirborne = false;
		pHunt->flJumpExpire = gpGlobals->time + BOT_HUNT_JUMP_TIMEOUT;

		move.buttons |= IN_JUMP;
	}

	move.flForward = BotHuntSpeed(pPlayer);
}

static void BotHuntThink(int index, CBasePlayer* pPlayer, byte msec)
{
	bothuntmove_t move;

	move.flYaw = g_flBotYaw[index];
	move.flForward = 0;
	move.buttons = 0;

	// Melee first, and it wins: something within crowbar reach outranks
	// something across the room, and while a swing is in progress the gun stays
	// put away.
	const bool bFighting = BotHuntMeleeThink(index, pPlayer, move);

	if (bFighting)
	{
		g_botHunt[index].gun = BotHuntGun::Idle;

		// Whatever it was jumping over matters less than the player standing
		// on top of it.
		g_botHunt[index].move = BotHuntMove::Wander;
		g_botHunt[index].bAirborne = false;
	}
	else
	{
		BotHuntGunThink(index, pPlayer, move);
	}

	BotHuntMoveThink(index, pPlayer, move, bFighting);

	const Vector vecAngles(0, move.flYaw, 0);

	// Face where it is going, so other players see the bot turn.
	pPlayer->pev->angles = vecAngles;
	pPlayer->pev->v_angle = vecAngles;

	g_engfuncs.pfnRunPlayerMove(pPlayer->edict(), vecAngles, move.flForward, 0, 0, static_cast<unsigned short>(move.buttons), 0, msec);
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
	g_bBotJumping[index] = false;

	BotHuntReset(index);

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
			// Nothing half-finished survives being frozen or dying: a bot that
			// comes back has no swing in progress and is not mid-jump.
			BotHuntReset(i);

			g_engfuncs.pfnRunPlayerMove(pPlayer->edict(), pPlayer->pev->v_angle, 0, 0, 0, 0, 0, msec);
			continue;
		}

		if (0 != bot_hunt.value)
		{
			BotHuntThink(i, static_cast<CBasePlayer*>(pPlayer), msec);
			continue;
		}

		// Everything from here down is the original wander, unchanged, and is
		// what runs with bot_hunt 0. Clear the hunting state on the way past so
		// that turning bot_hunt back on does not resume a jump the bot gave up
		// on several rounds ago.
		BotHuntReset(i);

		if (gpGlobals->time >= g_flBotNextProgressCheck[i])
		{
			if ((pPlayer->pev->origin - g_vecBotLastOrigin[i]).Length2D() < BOT_PROGRESS_DISTANCE)
			{
				if (!g_bBotJumping[i])
				{
					// Stuck for the first time on this yaw: try a crouch jump
					// in place rather than immediately abandoning the direction.
					g_bBotJumping[i] = true;
					g_vecBotLastOrigin[i] = pPlayer->pev->origin;
					g_flBotNextProgressCheck[i] = gpGlobals->time + BOT_JUMP_ATTEMPT_DURATION;
				}
				else
				{
					g_bBotJumping[i] = false;
					BotPickNewDirection(i, pPlayer);
				}
			}
			else
			{
				g_bBotJumping[i] = false;
				g_vecBotLastOrigin[i] = pPlayer->pev->origin;
				g_flBotNextProgressCheck[i] = gpGlobals->time + BOT_PROGRESS_INTERVAL;
			}
		}

		const Vector vecAngles(0, g_flBotYaw[i], 0);

		// Face where it is going, so other players see the bot turn.
		pPlayer->pev->angles = vecAngles;
		pPlayer->pev->v_angle = vecAngles;

		const float flSpeed = pPlayer->pev->maxspeed > 0 ? pPlayer->pev->maxspeed : 240.0f;
		const int buttons = g_bBotJumping[i] ? (IN_JUMP | IN_DUCK) : 0;

		g_engfuncs.pfnRunPlayerMove(pPlayer->edict(), vecAngles, flSpeed, 0, 0, 0, buttons, msec);
	}
}
