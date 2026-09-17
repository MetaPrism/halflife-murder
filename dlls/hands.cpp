/***
*
*	Crowbar Hunt: empty hands.
*
****/

#include "extdll.h"
#include "util.h"
#include "cbase.h"
#include "monsters.h"
#include "weapons.h"
#include "player.h"
#include "gamerules.h"
#include "game.h"

// auto weapon switch will default to hands
#define HANDS_WEIGHT 20

// A weapon that is nothing but a way to put your weapon away.
//
// Half-Life has no "holstered" state a player can sit in - CBasePlayer always
// deploys something - so hiding what you are carrying means carrying an item
// that shows nothing. Deploy() clears both the viewmodel and the third-person
// weaponmodel, and nothing it does is ever animated, so from the outside a
// player holding this is indistinguishable from a Survivor with nothing at all.
// That is the point: the Killer cannot be picked out by the crowbar in his
// hands, and the Hunter can keep the revolver a secret until the moment he
// uses it.
//
// The one thing it can do is punch a func_breakable (ch_handsbreak), so that
// windows are a route for everyone and not just the two players with a weapon.
// Nothing else takes damage from it, and there is no swing, whiff or hit
// feedback of any kind - the only sound is the breakable's own material noise,
// which CBreakable::TakeDamage() plays itself.
//
// Unlike the other weapons this one is not compiled into the client DLL. There
// is nothing to predict - no fire rate, no animations, no events - and
// HUD_WeaponsPostThink() simply bows out for a weapon id it doesn't know,
// leaving the (identical) server state to drive everything.
class CHands : public CBasePlayerWeapon
{
public:
	void Spawn() override;
	void Precache() override;
	int iItemSlot() override { return 1; }
	bool GetItemInfo(ItemInfo* p) override;

	bool Deploy() override;

	// Weapon timers are countdowns under CLIENT_WEAPONS, and the player only
	// ticks them down for weapons that say so. Without this the punch cadence
	// never elapses and GetNextAttackDelay()'s creep correction runs it
	// negative instead, so a held button punches every frame.
	bool UseDecrement() override
	{
#if defined(CLIENT_WEAPONS)
		return true;
#else
		return false;
#endif
	}

	void PrimaryAttack() override;

	// Deliberately inert. ItemPostFrame() will still call this while the
	// button is held; doing nothing is the whole behaviour.
	void SecondaryAttack() override {}
};

LINK_ENTITY_TO_CLASS(weapon_hands, CHands);

void CHands::Spawn()
{
	Precache();
	m_iId = WEAPON_HANDS;

	// Never actually meant to be seen lying around: this is only ever handed
	// out by GiveRoleLoadout(), which touches it straight onto the player. The
	// model is here because a weapon entity with no model has nothing for the
	// pickup trigger to size itself against.
	SET_MODEL(ENT(pev), "models/w_crowbar.mdl");
	pev->effects |= EF_NODRAW;

	m_iClip = -1;

	FallInit();
}

void CHands::Precache()
{
	PRECACHE_MODEL("models/w_crowbar.mdl");
}

bool CHands::GetItemInfo(ItemInfo* p)
{
	p->pszName = STRING(pev->classname);
	p->pszAmmo1 = NULL;
	p->iMaxAmmo1 = -1;
	p->pszAmmo2 = NULL;
	p->iMaxAmmo2 = -1;
	p->iMaxClip = WEAPON_NOCLIP;
	p->iSlot = 0;	 // hands is slot 1, weapons are slot 2
	p->iPosition = 1;
	p->iId = WEAPON_HANDS;

	// auto weapon switch goes to hands first
	p->iWeight = HANDS_WEIGHT;
	return true;
}

void CHands::PrimaryAttack()
{
	if (ch_handsbreak.value <= 0)
		return;

	// The crowbar's reach, and its cadence when it connects, so that a window
	// coming down sounds the same whoever is on the other side of it.
	UTIL_MakeVectors(m_pPlayer->pev->v_angle);
	Vector vecSrc = m_pPlayer->GetGunPosition();
	Vector vecEnd = vecSrc + gpGlobals->v_forward * 32;

	TraceResult tr;
	UTIL_TraceLine(vecSrc, vecEnd, dont_ignore_monsters, ENT(m_pPlayer->pev), &tr);
	if (tr.flFraction >= 1.0)
		UTIL_TraceHull(vecSrc, vecEnd, dont_ignore_monsters, head_hull, ENT(m_pPlayer->pev), &tr);

	m_flNextPrimaryAttack = GetNextAttackDelay(0.25);

	if (tr.flFraction >= 1.0)
		return;

	// Only a func_breakable, by name: a func_pushable is furniture, not a
	// route, and stays the crowbar's business. DMG_GENERIC rather than
	// DMG_CLUB so a punch gets neither the crowbar's double damage nor its
	// instant break on crowbar-only brushes - the crowbar stays the better
	// tool, and the Killer has to show it to get through faster.
	CBaseEntity* pEntity = CBaseEntity::Instance(tr.pHit);
	if (!pEntity || !FClassnameIs(pEntity->pev, "func_breakable"))
		return;

	ClearMultiDamage();
	pEntity->TraceAttack(m_pPlayer->pev, ch_handsbreak.value, gpGlobals->v_forward, &tr, DMG_GENERIC);
	ApplyMultiDamage(m_pPlayer->pev, m_pPlayer->pev);
}

bool CHands::Deploy()
{
	if (!CanDeploy())
		return false;

	m_pPlayer->TabulateAmmo();

	// The reason this weapon exists: nothing on screen, nothing in the hands.
	m_pPlayer->pev->viewmodel = 0;
	m_pPlayer->pev->weaponmodel = 0;

	// No empty-handed player animation set exists; the crowbar's is the closest
	// thing to a neutral one-armed stance.
	strcpy(m_pPlayer->m_szAnimExtention, "hive");

	m_pPlayer->m_flNextAttack = UTIL_WeaponTimeBase() + 0.5;
	m_flTimeWeaponIdle = UTIL_WeaponTimeBase() + 1.0;
	m_flLastFireTime = 0.0;

	return true;
}
