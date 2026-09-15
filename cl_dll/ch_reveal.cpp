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
// same way, typed out at the same rate as the result's effect-2 typewriter
// (both start together, so the two lines fill in side by side), held for the
// same time, and faded out over the same second, so the two leave together.
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

// Seconds per character. The HUD message's effect 2 shows one more character
// every fadeinTime; this is AnnounceRoundOver()'s fadeinTime.
constexpr float CH_REVEAL_CHAR_TIME = 0.05f;

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

// Draws at most `count` characters of psz and returns the x after them, which
// is always where the full string would have ended - so a run that is still
// typing out leaves the runs after it in their final place, unshown.
static int DrawRun(int x, int y, const char* psz, int count, int r, int g, int b)
{
	char szShown[MAX_PLAYER_NAME_LENGTH + 32];
	const int len = static_cast<int>(strlen(psz));
	const int shown = V_min(count, V_min(len, static_cast<int>(sizeof(szShown)) - 1));

	if (shown > 0)
	{
		memcpy(szShown, psz, shown);
		szShown[shown] = '\0';
		gHUD.DrawHudString(x, y, ScreenWidth, szShown, r, g, b);
	}

	return x + TextWidth(psz);
}

bool CHudCHReveal::Init()
{
	HOOK_MESSAGE(CHReveal);

	m_szRealName[0] = '\0';
	m_szAnonName[0] = '\0';
	m_iAnonColor = -1;
	m_flStartTime = 0.0f;
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

	// The hold runs from when the line has finished typing, as the HUD
	// message's does (fadein * length + holdtime), so a long name is not cut
	// short. The fixed text is "The Killer was " and either "." or " (" + ")."
	// around the anonymous name - the same strings Draw() builds.
	const int hold = READ_BYTE();
	const int length = static_cast<int>(strlen(m_szRealName) + strlen(m_szAnonName)) + (m_szAnonName[0] != '\0' ? 19 : 16);

	m_flStartTime = gHUD.m_flTime;
	m_flEndTime = hold > 0 ? gHUD.m_flTime + CH_REVEAL_CHAR_TIME * length + hold : 0.0f;

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

	const int leadLen = static_cast<int>(strlen(szLead));
	const int anonLen = pszAnon ? static_cast<int>(strlen(pszAnon)) : 0;

	// The typewriter: one more character every CH_REVEAL_CHAR_TIME, counted
	// across the whole line and handed to each run in turn.
	int shown = static_cast<int>((flTime - m_flStartTime) / CH_REVEAL_CHAR_TIME);
	if (shown <= 0)
		return true;

	// Fade the last second out by darkening: the engine's HUD text is
	// additive, so a darker colour is a fainter line.
	const int alpha = static_cast<int>(V_min(1.0f, remaining / CH_REVEAL_FADE_TIME) * 255.0f);

	int r = 255, g = 255, b = 255;
	ScaleColors(r, g, b, alpha);

	const int width = TextWidth(szLead) + (pszAnon ? TextWidth(pszAnon) : 0) + TextWidth(pszTail);

	int x = (ScreenWidth - width) / 2;
	const int y = static_cast<int>(CH_REVEAL_RESULT_Y * ScreenHeight) + gHUD.m_scrinfo.iCharHeight;

	x = DrawRun(x, y, szLead, shown, r, g, b);
	shown -= leadLen;

	if (pszAnon)
	{
		int ar = 255, ag = 255, ab = 255;
		if (m_iAnonColor >= 0)
		{
			const float* pflColor = g_CHAnonColors[m_iAnonColor].rgb;
			ar = static_cast<int>(pflColor[0] * 255.0f);
			ag = static_cast<int>(pflColor[1] * 255.0f);
			ab = static_cast<int>(pflColor[2] * 255.0f);
		}
		ScaleColors(ar, ag, ab, alpha);

		x = DrawRun(x, y, pszAnon, shown, ar, ag, ab);
		shown -= anonLen;

		DrawRun(x, y, pszTail, shown, r, g, b);
	}

	return true;
}
