//
// ch_loot.cpp
//
// Crowbar Hunt loot counter. Sits where the armour readout normally does and
// takes its place: armour is not part of the mode, so the slot is free, and a
// player already knows to look there for a number. CHudBattery stands aside
// while this is active.
//
// Activated by the first CHLoot message from the server, so on a server
// running some other mode it never shows and the armour readout is untouched.
//
// The icon is the "ch_loot" entry in sprites/hud.txt. Until a mod ships one,
// the suit icon stands in so the count is at least readable.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"

DECLARE_MESSAGE(m_CHLoot, CHLoot)

bool CHudCHLoot::Init()
{
	HOOK_MESSAGE(CHLoot);

	m_iCount = 0;
	m_fFade = 0;
	m_iFlags = 0;

	gHUD.AddHudElem(this);
	return true;
}

bool CHudCHLoot::VidInit()
{
	int index = gHUD.GetSpriteIndex("ch_loot");
	if (index < 0)
		index = gHUD.GetSpriteIndex("suit_full");

	m_hSprite = 0; // sprites are not loaded yet; fetched on first draw
	m_iSpriteIndex = index;
	m_fFade = 0;
	return true;
}

// A new server: whatever the last one left here is not this one's.
void CHudCHLoot::InitHUDData()
{
	m_iCount = 0;
	m_iFlags &= ~HUD_ACTIVE;
}

bool CHudCHLoot::MsgFunc_CHLoot(const char* pszName, int iSize, void* pbuf)
{
	m_iFlags |= HUD_ACTIVE;

	BEGIN_READ(pbuf, iSize);
	const int count = READ_SHORT();

	if (count != m_iCount)
	{
		m_fFade = FADE_TIME;
		m_iCount = count;
	}

	return true;
}

bool CHudCHLoot::Draw(float flTime)
{
	if ((gHUD.m_iHideHUDDisplay & HIDEHUD_HEALTH) != 0)
		return true;

	if (!gHUD.HasSuit())
		return true;

	if (m_iSpriteIndex < 0)
		return true;

	if (0 == m_hSprite)
		m_hSprite = gHUD.GetSprite(m_iSpriteIndex);

	const Rect& rc = gHUD.GetSpriteRect(m_iSpriteIndex);

	int r, g, b, a;
	UnpackRGB(r, g, b, gHUD.m_iHUDColor);

	// Flash the number when it changes, the way health and armour do.
	if (0 != m_fFade)
	{
		if (m_fFade > FADE_TIME)
			m_fFade = FADE_TIME;

		m_fFade -= (gHUD.m_flTimeDelta * 20);
		if (m_fFade <= 0)
			m_fFade = 0;

		a = MIN_ALPHA + (m_fFade / FADE_TIME) * 128;
	}
	else
		a = MIN_ALPHA;

	ScaleColors(r, g, b, a);

	// Same placement arithmetic as CHudBattery::Draw(), so the readout lands
	// exactly where the armour one would.
	const int width = rc.right - rc.left;
	const int iOffset = (rc.bottom - rc.top) / 6;

	int x = 3 * width;
	int y = ScreenHeight - gHUD.m_iFontHeight - gHUD.m_iFontHeight / 2;

	SPR_Set(m_hSprite, r, g, b);
	SPR_DrawAdditive(0, x, y - iOffset, &rc);

	x += width;
	y += (int)(gHUD.m_iFontHeight * 0.2f);
	gHUD.DrawHudNumber(x, y, DHN_3DIGITS | DHN_DRAWZERO, m_iCount, r, g, b);

	return true;
}
