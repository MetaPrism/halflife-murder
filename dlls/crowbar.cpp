/***
*
*	Copyright (c) 1996-2001, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"

#ifndef CLIENT_DLL
#include "game.h"
#include "crowbar_hunt_gamerules.h"
#endif


#define CROWBAR_BODYHIT_VOLUME 128
#define CROWBAR_WALLHIT_VOLUME 512

LINK_ENTITY_TO_CLASS(weapon_crowbar, CCrowbar);

void CCrowbar::Spawn()
{
	Precache();
	m_iId = WEAPON_CROWBAR;
	SET_MODEL(ENT(pev), "models/w_crowbar.mdl");
	m_iClip = -1;

	FallInit(); // get ready to fall down.
}


void CCrowbar::Precache()
{
	PRECACHE_MODEL("models/v_crowbar.mdl");
	PRECACHE_MODEL("models/w_crowbar.mdl");
	PRECACHE_MODEL("models/p_crowbar.mdl");
	PRECACHE_SOUND("weapons/cbar_hit1.wav");
	PRECACHE_SOUND("weapons/cbar_hit2.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod1.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod2.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod3.wav");
	PRECACHE_SOUND("weapons/cbar_miss1.wav");

#ifndef CLIENT_DLL
	// The thrown bar's own Precache() only runs when it is spawned, mid-round,
	// which is too late to add anything new to the precache table.
	UTIL_PrecacheOther("crowbar_thrown");
#endif

	m_usCrowbar = PRECACHE_EVENT(1, "events/crowbar.sc");
}

bool CCrowbar::GetItemInfo(ItemInfo* p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = NULL;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 1; // changed weapon category to same as 357
	p->iPosition = 0;
	p->iId = WEAPON_CROWBAR;
	p->iWeight = CROWBAR_WEIGHT;
	return true;
}



bool CCrowbar::Deploy()
{
	return DefaultDeploy("models/v_crowbar.mdl", "models/p_crowbar.mdl", CROWBAR_DRAW, "crowbar");
}

void CCrowbar::Holster()
{
	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5;
	SendWeaponAnim(CROWBAR_HOLSTER);
}


void FindHullIntersection(const Vector& vecSrc, TraceResult& tr, const Vector& mins, const Vector& maxs, edict_t* pEntity)
{
	int i, j, k;
	float distance;
	const Vector* minmaxs[2] = {&mins, &maxs};
	TraceResult tmpTrace;
	Vector vecHullEnd = tr.vecEndPos;
	Vector vecEnd;

	distance = 1e6f;

	vecHullEnd = vecSrc + ((vecHullEnd - vecSrc) * 2);
	UTIL_TraceLine(vecSrc, vecHullEnd, dont_ignore_monsters, pEntity, &tmpTrace);
	if (tmpTrace.flFraction < 1.0)
	{
		tr = tmpTrace;
		return;
	}

	for (i = 0; i < 2; i++)
	{
		for (j = 0; j < 2; j++)
		{
			for (k = 0; k < 2; k++)
			{
				vecEnd.x = vecHullEnd.x + minmaxs[i]->x;
				vecEnd.y = vecHullEnd.y + minmaxs[j]->y;
				vecEnd.z = vecHullEnd.z + minmaxs[k]->z;

				UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, pEntity, &tmpTrace);
				if (tmpTrace.flFraction < 1.0)
				{
					float thisDistance = (tmpTrace.vecEndPos - vecSrc).Length();
					if (thisDistance < distance)
					{
						tr = tmpTrace;
						distance = thisDistance;
					}
				}
			}
		}
	}
}


#ifndef CLIENT_DLL

// Crowbar Hunt: mouse2 throws the crowbar. The thrown bar hits for the same
// damage as a swing - a one-shot kill on a player - and then drops to the floor
// as a pickup only the thrower can collect, so the Killer is empty-handed until
// they walk back to it. Server-only: the client never predicts the throw.
constexpr float CROWBAR_THROW_SPEED = 1100.0f;
constexpr float CROWBAR_THROW_LIFT = 100.0f;
constexpr float CROWBAR_THROW_GRAVITY = 0.6f;
constexpr float CROWBAR_THROW_SPIN = -1500.0f;

// The trail the bar draws behind itself in flight: a TE_BEAMFOLLOW ribbon,
// the same effect as the RPG rocket's smoke, tinted to match the bar's glow
// shell. Life is in tenths of a second; short, so it reads as a streak that
// fades right behind the bar rather than a smoke column hanging in the room.
constexpr int CROWBAR_TRAIL_LIFE = 6;
constexpr int CROWBAR_TRAIL_WIDTH = 3;
constexpr int CROWBAR_TRAIL_BRIGHTNESS = 200;

// The bar flies as an 8-unit cube but lands in the standard 32x32x16 weapon
// pickup box - and the engine rounds that up again to one of its fixed
// collision hulls, which is larger still. A bar that stops flush against a
// wall or ceiling therefore lands with that box buried in the brush, and
// MOVETYPE_TOSS only works it loose a few units per frame: that is the bar
// visibly dragging itself out of the surface before it starts to fall.
// Stepping the origin back along the surface normal first drops it into open
// space, so it just falls.
constexpr float CROWBAR_LAND_CLEARANCE = 2.0f; // margin past the box itself
constexpr float CROWBAR_LAND_PROBE = 32.0f;    // how far ahead to look for the surface
constexpr float CROWBAR_LAND_BACKOFF = 8.0f;   // and how far behind it to start looking

class CCrowbarProjectile : public CBaseAnimating
{
public:
	void Spawn() override;
	void Precache() override;

	bool Save(CSave& save) override;
	bool Restore(CRestore& restore) override;
	static TYPEDESCRIPTION m_SaveData[];

	void EXPORT FlyTouch(CBaseEntity* pOther);
	void EXPORT PickupTouch(CBaseEntity* pOther);
	void EXPORT ReturnThink();

	// Launches a thrown crowbar from pOwner. Returns the projectile, or null if
	// the engine had no edict left for it.
	static CCrowbarProjectile* Throw(CBasePlayer* pOwner, const Vector& vecOrigin, const Vector& vecVelocity);

private:
	void Land();

	// Offset that would lift the landed pickup box clear of whatever the bar
	// just hit, or zero if it needs none.
	Vector GetLandClearance(const Vector& vecMins, const Vector& vecMaxs) const;

	// Hands the bar back to the thrower and removes it. No-op if they cannot
	// take it (gone, dead, or already holding a crowbar).
	bool GiveBackToThrower();

	// Removes the bar, first detaching the trail from its edict. The client
	// keeps a followed beam bound to the entity index, so without this a later
	// entity that reuses the slot would inherit the tail.
	void RemoveBar();

	EHANDLE m_hThrower;
	static int m_iTrail;
};

int CCrowbarProjectile::m_iTrail = 0;

LINK_ENTITY_TO_CLASS(crowbar_thrown, CCrowbarProjectile);

TYPEDESCRIPTION CCrowbarProjectile::m_SaveData[] = {
	DEFINE_FIELD(CCrowbarProjectile, m_hThrower, FIELD_EHANDLE),
};

IMPLEMENT_SAVERESTORE(CCrowbarProjectile, CBaseAnimating);

void CCrowbarProjectile::Precache()
{
	PRECACHE_MODEL("models/w_crowbar.mdl");
	PRECACHE_SOUND("weapons/cbar_hit1.wav");
	PRECACHE_SOUND("weapons/cbar_hitbod1.wav");
	PRECACHE_SOUND("items/gunpickup2.wav");
	m_iTrail = PRECACHE_MODEL("sprites/smoke.spr");
}

void CCrowbarProjectile::Spawn()
{
	Precache();

	pev->movetype = MOVETYPE_TOSS;
	pev->solid = SOLID_BBOX;
	pev->gravity = CROWBAR_THROW_GRAVITY;
	pev->friction = 0.8;

	SET_MODEL(ENT(pev), "models/w_crowbar.mdl");
	UTIL_SetSize(pev, Vector(-4, -4, -4), Vector(4, 4, 4));
	UTIL_SetOrigin(pev, pev->origin);

	SetTouch(&CCrowbarProjectile::FlyTouch);

	// On from the moment it leaves the Killer's hands: a red bar spinning
	// through the air is fair warning, and the same shell marks it on the floor
	// afterwards. Move this to Land() to only light it up once it settles.
	CH_SetWeaponGlow(this, CHWeaponGlow::Crowbar);
}

CCrowbarProjectile* CCrowbarProjectile::Throw(CBasePlayer* pOwner, const Vector& vecOrigin, const Vector& vecVelocity)
{
	// pev->owner is the thrower, so the engine skips collision between the two
	// while the bar is leaving their hands. Land() clears it again.
	CCrowbarProjectile* pBar = (CCrowbarProjectile*)CBaseEntity::Create(
		"crowbar_thrown", vecOrigin, pOwner->pev->v_angle, pOwner->edict());

	if (!pBar)
		return nullptr;

	pBar->pev->velocity = vecVelocity;
	pBar->pev->avelocity = Vector(CROWBAR_THROW_SPIN, 0, 0); // end over end
	pBar->m_hThrower = pOwner;

	// The trail stops growing on its own once the bar comes to rest, so there
	// is nothing to switch off at Land(); the tail just fades out behind it.
	MESSAGE_BEGIN(MSG_BROADCAST, SVC_TEMPENTITY);
	WRITE_BYTE(TE_BEAMFOLLOW);
	WRITE_SHORT(pBar->entindex());
	WRITE_SHORT(m_iTrail);
	WRITE_BYTE(CROWBAR_TRAIL_LIFE);
	WRITE_BYTE(CROWBAR_TRAIL_WIDTH);
	WRITE_BYTE(200); // r, g, b: the crowbar glow colour from CH_SetWeaponGlow()
	WRITE_BYTE(20);
	WRITE_BYTE(20);
	WRITE_BYTE(CROWBAR_TRAIL_BRIGHTNESS);
	MESSAGE_END();

	// The bar finds its own way home after a while, so a throw that sails off a
	// cliff does not leave the Killer permanently unarmed. The clock starts now,
	// in flight - time spent lying on the floor counts against it too.
	if (ch_crowbar_return_time.value > 0)
	{
		pBar->SetThink(&CCrowbarProjectile::ReturnThink);
		pBar->pev->nextthink = gpGlobals->time + ch_crowbar_return_time.value;
	}

	return pBar;
}

void CCrowbarProjectile::FlyTouch(CBaseEntity* pOther)
{
	if (!pOther || pOther->edict() == pev->owner)
		return;

	if (DAMAGE_NO != pOther->pev->takedamage)
	{
		CBaseEntity* pThrower = m_hThrower;

		// Attribute the kill to whoever threw it; fall back to the bar itself if
		// they have already disconnected.
		pOther->TakeDamage(pev, pThrower ? pThrower->pev : pev, gSkillData.plrDmgCrowbar, DMG_CLUB);

		if (pOther->IsPlayer() || (pOther->Classify() != CLASS_NONE && pOther->Classify() != CLASS_MACHINE))
			EMIT_SOUND(ENT(pev), CHAN_WEAPON, "weapons/cbar_hitbod1.wav", 1, ATTN_NORM);
		else
			EMIT_SOUND(ENT(pev), CHAN_WEAPON, "weapons/cbar_hit1.wav", 1, ATTN_NORM);
	}
	else
	{
		EMIT_SOUND_DYN(ENT(pev), CHAN_WEAPON, "weapons/cbar_hit1.wav", 1, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));
	}

	Land();
}

void CCrowbarProjectile::Land()
{
	// Same physics setup CBasePlayerItem::FallInit() uses for a dropped weapon:
	// falls to the floor, then sits there as a trigger waiting to be walked over.
	const Vector vecLandMins(-16, -16, 0);
	const Vector vecLandMaxs(16, 16, 16);

	// Measured first: the velocity below is what points at the surface, and the
	// next two lines throw it away.
	const Vector vecClearance = GetLandClearance(vecLandMins, vecLandMaxs);

	pev->owner = nullptr; // the thrower has to be able to touch it now
	pev->velocity = g_vecZero;
	pev->avelocity = g_vecZero;
	pev->angles.x = 0;
	pev->angles.z = 0;
	pev->movetype = MOVETYPE_TOSS;
	pev->solid = SOLID_TRIGGER;

	UTIL_SetSize(pev, vecLandMins, vecLandMaxs);
	UTIL_SetOrigin(pev, pev->origin + vecClearance);

	SetTouch(&CCrowbarProjectile::PickupTouch);
}

// Called from Land(), and only from there: it reads pev->velocity, which is
// still the incoming velocity at that point. The engine runs an entity's touch
// callback from inside SV_PushEntity, before SV_Physics_Toss clips the velocity
// to the surface it hit, so the flight path is a reliable pointer at the plane -
// but only until Land() zeroes it.
Vector CCrowbarProjectile::GetLandClearance(const Vector& vecMins, const Vector& vecMaxs) const
{
	if (pev->velocity.Length() < 1)
		return g_vecZero;

	const Vector vecDir = pev->velocity.Normalize();

	TraceResult tr;

	// Started behind the impact point rather than on it: the bar stops flush
	// against the plane, and a trace beginning exactly on a surface can come
	// back start-solid with no usable normal. A few units back down the flight
	// path is somewhere the bar provably was a moment ago, so it is open space.
	//
	// ignore_monsters: a bar that hit a player carries on to whatever is really
	// behind them. A body is not something it can get wedged in, so if there is
	// nothing solid back there it needs no clearance at all.
	UTIL_TraceLine(pev->origin - vecDir * CROWBAR_LAND_BACKOFF,
		pev->origin + vecDir * CROWBAR_LAND_PROBE, ignore_monsters, ENT(pev), &tr);

	if (0 != tr.fAllSolid || 1.0f == tr.flFraction)
		return g_vecZero;

	const Vector vecNormal = tr.vecPlaneNormal;

	// How far the landed box reaches from its own origin towards that surface.
	// Done per axis because the box is not centred on the origin - its mins are
	// flush with the origin in z, which is what makes an ordinary floor landing
	// need no clearance and leaves this alone.
	const float flReach =
		fabs(vecNormal.x) * (vecNormal.x > 0 ? -vecMins.x : vecMaxs.x) +
		fabs(vecNormal.y) * (vecNormal.y > 0 ? -vecMins.y : vecMaxs.y) +
		fabs(vecNormal.z) * (vecNormal.z > 0 ? -vecMins.z : vecMaxs.z);

	// ...against the room it already has, measured from the plane it stopped on.
	const float flHave = DotProduct(pev->origin - tr.vecEndPos, vecNormal);
	const float flPush = flReach + CROWBAR_LAND_CLEARANCE - flHave;

	if (flPush <= 0)
		return g_vecZero;

	// Always roughly back down the flight path, into space the bar has just
	// flown through, so this cannot shove it somewhere it could not reach.
	return vecNormal * flPush;
}

void CCrowbarProjectile::PickupTouch(CBaseEntity* pOther)
{
	// Only the Killer who threw it gets it back - a Survivor who stumbles over
	// the bar is not allowed to arm themselves with it.
	if (!pOther || !pOther->IsPlayer() || !pOther->IsAlive())
		return;

	if (static_cast<CBaseEntity*>(m_hThrower) != pOther)
		return;

	GiveBackToThrower();
}

bool CCrowbarProjectile::GiveBackToThrower()
{
	CBaseEntity* pThrower = m_hThrower;

	if (!pThrower || !pThrower->IsPlayer() || !pThrower->IsAlive())
		return false;

	CBasePlayer* pPlayer = (CBasePlayer*)pThrower;

	if (pPlayer->HasNamedPlayerItem("weapon_crowbar"))
		return false;

	pPlayer->GiveNamedItem("weapon_crowbar");
	EMIT_SOUND(ENT(pPlayer->pev), CHAN_ITEM, "items/gunpickup2.wav", 1, ATTN_NORM);

	RemoveBar();
	return true;
}

void CCrowbarProjectile::ReturnThink()
{
	if (GiveBackToThrower())
		return;

	// The thrower is dead, gone, or somehow already armed - the bar is nobody's
	// pickup any more, so it just disappears.
	RemoveBar();
}

void CCrowbarProjectile::RemoveBar()
{
	MESSAGE_BEGIN(MSG_BROADCAST, SVC_TEMPENTITY);
	WRITE_BYTE(TE_KILLBEAM);
	WRITE_SHORT(entindex());
	MESSAGE_END();

	UTIL_Remove(this);
}

#endif

void CCrowbar::PrimaryAttack()
{
	if (!Swing(true))
	{
		SetThink(&CCrowbar::SwingAgain);
		pev->nextthink = gpGlobals->time + 0.1;
	}
}


void CCrowbar::SecondaryAttack()
{
#ifndef CLIENT_DLL
	// Throwing is a Crowbar Hunt mechanic only; every other mode keeps the stock
	// crowbar, which has no secondary fire.
	if (0 == sv_crowbarhunt.value)
		return;

	UTIL_MakeVectors(m_pPlayer->pev->v_angle);

	Vector vecSrc = m_pPlayer->GetGunPosition() + gpGlobals->v_forward * 16;
	Vector vecVelocity = gpGlobals->v_forward * CROWBAR_THROW_SPEED + gpGlobals->v_up * CROWBAR_THROW_LIFT + m_pPlayer->pev->velocity;

	CCrowbarProjectile::Throw(m_pPlayer, vecSrc, vecVelocity);

	m_pPlayer->SetAnimation(PLAYER_ATTACK1);
	SendWeaponAnim(CROWBAR_ATTACK1MISS);
	EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_WEAPON, "weapons/cbar_miss1.wav", 1, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));

	// The bar is gone: strip the weapon so the Killer is empty-handed until they
	// retrieve it. Deferred a tick because RemovePlayerItem() holsters the active
	// item, which is the call we are inside of right now.
	m_pPlayer->ClearWeaponBit(m_iId);
	SetThink(&CCrowbar::RemoveAfterThrow);
	pev->nextthink = gpGlobals->time + 0.1;
#endif

	m_flNextPrimaryAttack = m_flNextSecondaryAttack = GetNextAttackDelay(0.5);
}


#ifndef CLIENT_DLL

void CCrowbar::RemoveAfterThrow()
{
	CBasePlayer* pPlayer = m_pPlayer;

	if (pPlayer)
	{
		pPlayer->RemovePlayerItem(this);

		// RemovePlayerItem() leaves the player holding literally nothing: no
		// active item at all. That reads wrong in two ways - they keep the
		// crowbar's third-person stance, and with m_pActiveItem null
		// FShouldSwitchWeapon() auto-equips the bar the instant they walk back
		// over it. Empty hands fixes both, and since hands outweigh the crowbar
		// picking the bar back up no longer switches for them either.
		pPlayer->SelectItem("weapon_hands");
	}

	Kill();
}

#endif

void CCrowbar::Smack()
{
	DecalGunshot(&m_trHit, BULLET_PLAYER_CROWBAR);
}


void CCrowbar::SwingAgain()
{
	Swing(false);
}


bool CCrowbar::Swing(bool fFirst)
{
	bool fDidHit = false;

	TraceResult tr;

	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecEnd = vecSrc + gpGlobals->v_forward * 32;

	UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, ENT(m_pPlayer->pev), &tr);

#ifndef CLIENT_DLL
	if (tr.flFraction >= 1.0)
	{
		UTIL_TraceHull(vecSrc, vecEnd, dont_ignore_monsters, head_hull, ENT(m_pPlayer->pev), &tr);
		if (tr.flFraction < 1.0)
		{
			// Calculate the point of intersection of the line (or hull) and the object we hit
			// This is and approximation of the "best" intersection
			CBaseEntity* pHit = CBaseEntity::Instance(tr.pHit);
			if (!pHit || pHit->IsBSPModel())
				FindHullIntersection(vecSrc, tr, VEC_DUCK_HULL_MIN, VEC_DUCK_HULL_MAX, m_pPlayer->edict());
			vecEnd = tr.vecEndPos; // This is the point on the actual surface (the hull could have hit space)
		}
	}
#endif

	if (fFirst)
	{
		PLAYBACK_EVENT_FULL(FEV_NOTHOST, m_pPlayer->edict(), m_usCrowbar,
			0.0, g_vecZero, g_vecZero, 0, 0, 0,
			0.0, 0, 0.0);
	}


	if (tr.flFraction >= 1.0)
	{
		if (fFirst)
		{
			// miss
			m_flNextPrimaryAttack = GetNextAttackDelay(0.5);

			// player "shoot" animation
			m_pPlayer->SetAnimation(PLAYER_ATTACK1);
		}
	}
	else
	{
		switch (((m_iSwing++) % 2) + 1)
		{
		case 0:
			SendWeaponAnim(CROWBAR_ATTACK1HIT);
			break;
		case 1:
			SendWeaponAnim(CROWBAR_ATTACK2HIT);
			break;
		case 2:
			SendWeaponAnim(CROWBAR_ATTACK3HIT);
			break;
		}

		// player "shoot" animation
		m_pPlayer->SetAnimation(PLAYER_ATTACK1);

#ifndef CLIENT_DLL

		// hit
		fDidHit = true;
		CBaseEntity* pEntity = CBaseEntity::Instance(tr.pHit);

		ClearMultiDamage();

		// JoshA: Changed from < -> <= to fix the full swing logic since client weapon prediction.
		// -1.0f + 1.0f = 0.0f. UTIL_WeaponTimeBase is always 0 with client weapon prediction (0 time base vs curtime base)
		if ((m_flNextPrimaryAttack + 1.0f <= UTIL_WeaponTimeBase()) || g_pGameRules->IsMultiplayer())
		{
			// first swing does full damage
			pEntity->TraceAttack(m_pPlayer->pev, gSkillData.plrDmgCrowbar, gpGlobals->v_forward, &tr, DMG_CLUB);
		}
		else
		{
			// subsequent swings do half
			pEntity->TraceAttack(m_pPlayer->pev, gSkillData.plrDmgCrowbar / 2, gpGlobals->v_forward, &tr, DMG_CLUB);
		}
		ApplyMultiDamage(m_pPlayer->pev, m_pPlayer->pev);

#endif

		m_flNextPrimaryAttack = GetNextAttackDelay(0.25);

#ifndef CLIENT_DLL
		// play thwack, smack, or dong sound
		float flVol = 1.0;
		bool fHitWorld = true;

		if (pEntity)
		{
			if (pEntity->Classify() != CLASS_NONE && pEntity->Classify() != CLASS_MACHINE)
			{
				// play thwack or smack sound
				switch (RANDOM_LONG(0, 2))
				{
				case 0:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hitbod1.wav", 1, ATTN_NORM);
					break;
				case 1:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hitbod2.wav", 1, ATTN_NORM);
					break;
				case 2:
					EMIT_SOUND(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hitbod3.wav", 1, ATTN_NORM);
					break;
				}
				m_pPlayer->m_iWeaponVolume = CROWBAR_BODYHIT_VOLUME;
				if (!pEntity->IsAlive())
					return true;
				else
					flVol = 0.1;

				fHitWorld = false;
			}
		}

		// play texture hit sound
		// UNDONE: Calculate the correct point of intersection when we hit with the hull instead of the line

		if (fHitWorld)
		{
			float fvolbar = TEXTURETYPE_PlaySound(&tr, vecSrc, vecSrc + (vecEnd - vecSrc) * 2, BULLET_PLAYER_CROWBAR);

			if (g_pGameRules->IsMultiplayer())
			{
				// override the volume here, cause we don't play texture sounds in multiplayer,
				// and fvolbar is going to be 0 from the above call.

				fvolbar = 0.5;
			}

			// also play crowbar strike
			switch (RANDOM_LONG(0, 1))
			{
			case 0:
				EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hit1.wav", fvolbar, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));
				break;
			case 1:
				EMIT_SOUND_DYN(ENT(m_pPlayer->pev), CHAN_ITEM, "weapons/cbar_hit2.wav", fvolbar, ATTN_NORM, 0, 98 + RANDOM_LONG(0, 3));
				break;
			}

			// delay the decal a bit
			m_trHit = tr;
		}

		m_pPlayer->m_iWeaponVolume = flVol * CROWBAR_WALLHIT_VOLUME;
#endif
		SetThink(&CCrowbar::Smack);
		pev->nextthink = gpGlobals->time + 0.2;
	}
	return fDidHit;
}
