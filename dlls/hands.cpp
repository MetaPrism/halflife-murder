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

// auto weapon switch will default to hands
#define HANDS_WEIGHT 20

// A weapon that is nothing but a way to put your weapon away.
//
// Half-Life has no "holstered" state a player can sit in - CBasePlayer always
// deploys something - so hiding what you are carrying means carrying an item
// that shows nothing. Deploy() clears both the viewmodel and the third-person
// weaponmodel, and neither attack does anything, so from the outside a player
// holding this is indistinguishable from a Survivor with nothing at all. That
// is the point: the Killer cannot be picked out by the crowbar in his hands,
// and the Hunter can keep the revolver a secret until the moment he uses it.
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

	// Deliberately inert. ItemPostFrame() will still call these while the
	// button is held; doing nothing is the whole behaviour.
	void PrimaryAttack() override {}
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
