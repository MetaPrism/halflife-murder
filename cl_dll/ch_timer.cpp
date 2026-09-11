//
// ch_timer.cpp
//
// Crowbar Hunt round clock. The server sends the seconds left once, when the
// round goes live (and again to anyone who joins mid-round); from there the
// clock counts down on the client's own time and hides itself at zero. A send
// of 0 takes it off the screen straight away - the round ended some other way.
//
// Drawn as mm:ss in the HUD number sprites, centred along the bottom edge on
// the same baseline as the health and ammo readouts.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"

#include <math.h>

DECLARE_MESSAGE(m_CHTimer, CHTimer)

// The clock turns red once this many seconds are left.
constexpr int CH_TIMER_WARNING_SECONDS = 30;

// Always two digits, zero-padded. gHUD.DrawHudNumber() leaves the tens slot
// blank for values under ten, which reads as "4: 5" on a clock.
static int DrawTwoDigits(int x, int y, int value, int r, int g, int b)
{
	const int digitWidth = gHUD.GetSpriteRect(gHUD.m_HUD_number_0).right - gHUD.GetSpriteRect(gHUD.m_HUD_number_0).left;
	const int digits[2] = {(value / 10) % 10, value % 10};

	for (int digit : digits)
	{
		SPR_Set(gHUD.GetSprite(gHUD.m_HUD_number_0 + digit), r, g, b);
		SPR_DrawAdditive(0, x, y, &gHUD.GetSpriteRect(gHUD.m_HUD_number_0 + digit));
		x += digitWidth;
	}

	return x;
}

bool CHudCHTimer::Init()
{
	HOOK_MESSAGE(CHTimer);

	m_flEndTime = 0.0f;
	m_iFlags |= HUD_ACTIVE;

	gHUD.AddHudElem(this);
	return true;
}

bool CHudCHTimer::VidInit()
{
	return true;
}

// A new server connection: whatever clock the last one left running is stale.
// Deliberately not Reset() - that fires on every respawn, and the round is
// still going when a player respawns into it.
void CHudCHTimer::InitHUDData()
{
	m_flEndTime = 0.0f;
}

bool CHudCHTimer::MsgFunc_CHTimer(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	const int seconds = READ_SHORT();

	m_flEndTime = seconds > 0 ? gHUD.m_flTime + seconds : 0.0f;
	return true;
}

bool CHudCHTimer::Draw(float flTime)
{
	if ((gHUD.m_iHideHUDDisplay & HIDEHUD_ALL) != 0)
		return true;

	if (m_flEndTime == 0.0f)
		return true;

	const int remaining = static_cast<int>(ceilf(m_flEndTime - flTime));
	if (remaining <= 0)
	{
		m_flEndTime = 0.0f;
		return true;
	}

	const int minutes = remaining / 60;
	const int seconds = remaining % 60;

	int r, g, b;
	if (remaining <= CH_TIMER_WARNING_SECONDS)
		UnpackRGB(r, g, b, RGB_REDISH);
	else
		UnpackRGB(r, g, b, gHUD.m_iHUDColor);
	ScaleColors(r, g, b, 255);

	const int digitWidth = gHUD.GetSpriteRect(gHUD.m_HUD_number_0).right - gHUD.GetSpriteRect(gHUD.m_HUD_number_0).left;
	const int fontHeight = gHUD.m_iFontHeight;

	// The colon is two filled squares - there is no colon glyph in the number
	// sprite set - sized off the digit height so it scales with the resolution.
	const int dot = V_max(2, fontHeight / 8);
	const int colonWidth = dot * 3;

	// mm:ss is two digits either side of the colon; centre the whole run.
	const int totalWidth = digitWidth * 4 + colonWidth;
	int x = (ScreenWidth - totalWidth) / 2;
	const int y = ScreenHeight - fontHeight - fontHeight / 2;

	x = DrawTwoDigits(x, y, minutes, r, g, b);

	const int dotX = x + dot;
	FillRGBA(dotX, y + fontHeight / 3 - dot / 2, dot, dot, r, g, b, 255);
	FillRGBA(dotX, y + (fontHeight * 2) / 3 - dot / 2, dot, dot, r, g, b, 255);
	x += colonWidth;

	DrawTwoDigits(x, y, seconds, r, g, b);

	return true;
}
