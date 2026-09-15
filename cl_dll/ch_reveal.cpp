//
// ch_reveal.cpp
//
// Crowbar Hunt round-over reveal: "The Killer was <name> (<anonymous name>)."
// under the round result. The result itself is a HUD message
// (CHalfLifeCrowbarHunt::AnnounceRoundOver), which is one colour throughout;
// this line is drawn here so the anonymous name can wear the colour that
// player was tinted for the round - the one everybody spent the round
// looking at, so the reveal reads as "that one was them". Everything else on
// the line is the message's white.
//
// Drawn on the line directly below the result, in the same font, centred the
// same way, and held for the same time so the two leave the screen together.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "crowbar_hunt_shared.h"

#include <math.h>

DECLARE_MESSAGE(m_CHReveal, CHReveal)

// The round result's y, as a fraction of the screen. Must match the parms.y
// AnnounceRoundOver() sends the result at.
constexpr float CH_REVEAL_RESULT_Y = 0.7f;

// The last second before m_flEndTime fades the line out, like the result's
// fadeoutTime.
constexpr float CH_REVEAL_FADE_TIME = 1.0f;

static int TextWidth(const char* psz)
{
	int width = 0;
	for (; *psz; psz++)
		width += gHUD.m_scrinfo.charWidths[static_cast<unsigned char>(*psz)];
	return width;
}

bool CHudCHReveal::Init()
{
	HOOK_MESSAGE(CHReveal);

	m_szRealName[0] = '\0';
	m_szAnonName[0] = '\0';
	m_iAnonColor = -1;
	m_flEndTime = 0.0f;
	m_iFlags |= HUD_ACTIVE;

	gHUD.AddHudElem(this);
	return true;
}

bool CHudCHReveal::VidInit()
{
	return true;
}

// A new server connection: the last one's reveal is stale. Not Reset() - that
// fires on every respawn, and the round-end respawn lands while this is up.
void CHudCHReveal::InitHUDData()
{
	m_szRealName[0] = '\0';
	m_flEndTime = 0.0f;
}

bool CHudCHReveal::MsgFunc_CHReveal(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);

	const int color = READ_BYTE();
	m_iAnonColor = (color >= 0 && color < CH_NUM_ANON_COLORS) ? color : -1;

	strncpy(m_szRealName, READ_STRING(), sizeof(m_szRealName) - 1);
	m_szRealName[sizeof(m_szRealName) - 1] = '\0';

	strncpy(m_szAnonName, READ_STRING(), sizeof(m_szAnonName) - 1);
	m_szAnonName[sizeof(m_szAnonName) - 1] = '\0';

	const int hold = READ_BYTE();
	m_flEndTime = hold > 0 ? gHUD.m_flTime + hold : 0.0f;

	return true;
}

bool CHudCHReveal::Draw(float flTime)
{
	if ((gHUD.m_iHideHUDDisplay & HIDEHUD_ALL) != 0)
		return true;

	if (m_szRealName[0] == '\0' || m_flEndTime == 0.0f)
		return true;

	const float remaining = m_flEndTime - flTime;
	if (remaining <= 0.0f)
	{
		m_flEndTime = 0.0f;
		return true;
	}

	// Three runs: the lead-in and the real name in white, the anonymous name
	// in its colour, the closing bracket and full stop in white again. Only
	// the middle one exists when the Killer wore no anonymous identity.
	char szLead[MAX_PLAYER_NAME_LENGTH + 32];
	const char* pszAnon = m_szAnonName[0] != '\0' ? m_szAnonName : nullptr;
	const char* pszTail = pszAnon ? ")." : "";

	snprintf(szLead, sizeof(szLead), pszAnon ? "The Killer was %s (" : "The Killer was %s.", m_szRealName);

	const int width = TextWidth(szLead) + (pszAnon ? TextWidth(pszAnon) : 0) + TextWidth(pszTail);

	// Fade the last second out by darkening: the engine's HUD text is
	// additive, so a darker colour is a fainter line.
	const float alpha = V_min(1.0f, remaining / CH_REVEAL_FADE_TIME);

	int r = 255, g = 255, b = 255;
	ScaleColors(r, g, b, static_cast<int>(alpha * 255.0f));

	int x = (ScreenWidth - width) / 2;
	const int y = static_cast<int>(CH_REVEAL_RESULT_Y * ScreenHeight) + gHUD.m_scrinfo.iCharHeight;

	x = gHUD.DrawHudString(x, y, ScreenWidth, szLead, r, g, b);

	if (pszAnon)
	{
		const float* pflColor = m_iAnonColor >= 0 ? g_CHAnonColors[m_iAnonColor].rgb : nullptr;

		int ar = 255, ag = 255, ab = 255;
		if (pflColor)
		{
			ar = static_cast<int>(pflColor[0] * 255.0f);
			ag = static_cast<int>(pflColor[1] * 255.0f);
			ab = static_cast<int>(pflColor[2] * 255.0f);
		}
		ScaleColors(ar, ag, ab, static_cast<int>(alpha * 255.0f));

		x = gHUD.DrawHudString(x, y, ScreenWidth, pszAnon, ar, ag, ab);
		gHUD.DrawHudString(x, y, ScreenWidth, pszTail, r, g, b);
	}

	return true;
}
