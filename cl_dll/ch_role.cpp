//
// ch_role.cpp
//
// Crowbar Hunt role label. The server sends the local player's role when the
// round goes live (and to anyone who joins mid-round), and 0 when the round is
// reset or the player sits out; the label stays up as long as the last send
// was a real role. Drawn as text, centred along the top edge, in the same
// colour the role's announcement used.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"

DECLARE_MESSAGE(m_CHRole, CHRole)

// Values on the wire. Must match CHalfLifeCrowbarHunt::SendRoleHud().
enum
{
	CH_ROLE_HUD_NONE = 0,
	CH_ROLE_HUD_KILLER,
	CH_ROLE_HUD_HUNTER,
	CH_ROLE_HUD_SURVIVOR,
};

bool CHudCHRole::Init()
{
	HOOK_MESSAGE(CHRole);

	m_iRole = CH_ROLE_HUD_NONE;
	m_iFlags |= HUD_ACTIVE;

	gHUD.AddHudElem(this);
	return true;
}

bool CHudCHRole::VidInit()
{
	return true;
}

// A new server connection: the last one's role is stale. Not Reset() - that
// fires on every respawn, and the role survives a respawn into the same round.
void CHudCHRole::InitHUDData()
{
	m_iRole = CH_ROLE_HUD_NONE;
}

bool CHudCHRole::MsgFunc_CHRole(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_iRole = READ_BYTE();
	return true;
}

bool CHudCHRole::Draw(float flTime)
{
	if ((gHUD.m_iHideHUDDisplay & HIDEHUD_ALL) != 0)
		return true;

	const char* pszText;
	int r, g, b;

	// Colours match AnnounceRole() on the server.
	switch (m_iRole)
	{
	case CH_ROLE_HUD_KILLER:
		pszText = "KILLER";
		r = 200;
		g = 40;
		b = 40;
		break;

	case CH_ROLE_HUD_HUNTER:
		pszText = "HUNTER";
		r = 220;
		g = 170;
		b = 40;
		break;

	case CH_ROLE_HUD_SURVIVOR:
		pszText = "SURVIVOR";
		r = 200;
		g = 200;
		b = 200;
		break;

	default:
		return true;
	}

	// pfnDrawString draws left-to-right from x, so measure first to centre.
	int width = 0;
	for (const char* p = pszText; *p; p++)
		width += gHUD.m_scrinfo.charWidths[static_cast<unsigned char>(*p)];

	const int x = (ScreenWidth - width) / 2;
	const int y = gHUD.m_scrinfo.iCharHeight / 2;

	gHUD.DrawHudString(x, y, ScreenWidth, pszText, r, g, b);

	return true;
}
