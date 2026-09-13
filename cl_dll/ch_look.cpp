//
// ch_look.cpp
//
// Crowbar Hunt corpse label. The server watches what each player is looking
// at (CHalfLifeCrowbarHunt::ServiceCorpseLook) and sends the name and name
// colour a body died wearing when one comes under the crosshair, and an empty
// name when it leaves. Drawn just below the crosshair, so a glance at a body
// reads the same way a glance at a live player's model does: this is who the
// room knew them as, in the colour they wore - which for a disguised Killer's
// victims is exactly the lie the Killer told.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "crowbar_hunt_shared.h"

DECLARE_MESSAGE(m_CHLook, CHLook)

bool CHudCHLook::Init()
{
	HOOK_MESSAGE(CHLook);

	m_szName[0] = '\0';
	m_iAnonColor = -1;
	m_iFlags |= HUD_ACTIVE;

	gHUD.AddHudElem(this);
	return true;
}

bool CHudCHLook::VidInit()
{
	return true;
}

// A new server connection: whatever the last one had us looking at is gone.
void CHudCHLook::InitHUDData()
{
	m_szName[0] = '\0';
	m_iAnonColor = -1;
}

bool CHudCHLook::MsgFunc_CHLook(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);

	const int color = READ_BYTE();
	m_iAnonColor = (color >= 0 && color < CH_NUM_ANON_COLORS) ? color : -1;

	strncpy(m_szName, READ_STRING(), sizeof(m_szName) - 1);
	m_szName[sizeof(m_szName) - 1] = '\0';

	return true;
}

bool CHudCHLook::Draw(float flTime)
{
	if ((gHUD.m_iHideHUDDisplay & HIDEHUD_ALL) != 0)
		return true;

	if (m_szName[0] == '\0')
		return true;

	int r, g, b;

	if (m_iAnonColor >= 0)
	{
		// The same table the server picked from when it tinted the model, so
		// the label and the body agree.
		const float* rgb = g_CHAnonColors[m_iAnonColor].rgb;
		r = static_cast<int>(rgb[0] * 255.0f);
		g = static_cast<int>(rgb[1] * 255.0f);
		b = static_cast<int>(rgb[2] * 255.0f);
	}
	else
	{
		UnpackRGB(r, g, b, gHUD.m_iHUDColor);
	}

	// pfnDrawString draws left-to-right from x, so measure first to centre.
	int width = 0;
	for (const char* p = m_szName; *p; p++)
		width += gHUD.m_scrinfo.charWidths[static_cast<unsigned char>(*p)];

	const int x = (ScreenWidth - width) / 2;
	const int y = ScreenHeight / 2 + gHUD.m_scrinfo.iCharHeight * 2;

	gHUD.DrawHudString(x, y, ScreenWidth, m_szName, r, g, b);

	return true;
}
