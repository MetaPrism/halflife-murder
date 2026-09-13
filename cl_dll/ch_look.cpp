//
// ch_look.cpp
//
// Crowbar Hunt corpse label. The server watches what each player is looking
// at (CHalfLifeCrowbarHunt::ServiceCorpseLook) and sends the name and name
// colour a body died wearing when one comes under the crosshair, and an empty
// name when it leaves. Drawn in the same place and style as the status bar's
// ID line for a live player, so a glance at a body reads the same way a
// glance at a standing player does: this is who the room knew them as, in the
// colour they wore - which for a disguised Killer's victims is exactly the
// lie the Killer told.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "crowbar_hunt_shared.h"

extern float g_ColorYellow[3]; // death.cpp

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

	// Drawn exactly as CHudStatusBar draws its ID line for a live player -
	// console font, centred under the crosshair at the hud_centerid offset,
	// coloured the way GetClientColor() would colour that player - so a body
	// and a standing player read the same. The anonymous table is the one the
	// server tinted the model from; yellow is what GetClientColor() gives an
	// unteamed player with no identity.
	const float* pflColor = m_iAnonColor >= 0 ? g_CHAnonColors[m_iAnonColor].rgb : g_ColorYellow;

	int TextWidth, TextHeight;
	GetConsoleStringSize(m_szName, &TextWidth, &TextHeight);

	const int x = V_max(0, V_max(2, (ScreenWidth - TextWidth)) / 2);
	const int y = (ScreenHeight / 2) + (int)(TextHeight * V_max(2.0f, CVAR_GET_FLOAT("hud_centerid")));

	gEngfuncs.pfnDrawSetTextColor(pflColor[0], pflColor[1], pflColor[2]);
	DrawConsoleString(x, y, m_szName);

	return true;
}
