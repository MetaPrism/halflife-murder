//
// ch_proxvoice.cpp
//
// Crowbar Hunt distance falloff for voice chat. The server's ch_proxvoice mask
// is a hard cutoff: inside the radius a voice is heard at full volume, outside
// it not at all, so players popping in and out at full volume as they cross
// the edge reads as jarring. This fades other voices down with distance so the
// cutoff lands near silence.
//
// The engine offers no per-speaker gain - the only lever is OtherSpeakerScale
// (the player's own "voice_scale" option), which scales every other voice at
// once. So the nearest current speaker sets the volume for all of them: right
// while one person talks at a time, wrong in a many-voice standoff. The setting
// is the player's preference and persists to their config, so the baseline is
// captured before the first scale and put back the moment nobody is talking.
//
// Nothing here loosens the mask: a stock client.dll just hears everything at
// full volume within the radius, which is still what the server enforces.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "voice_status.h"
#include "ivoicetweak.h"

DECLARE_MESSAGE(m_CHProxVoice, CHProxVoice)

// Distance from the listener over which a voice stays at full volume. Beyond
// it the scale drops linearly to CH_PROXVOICE_FLOOR at the server's radius.
constexpr float CH_PROXVOICE_FULL_FRACTION = 0.25f;
constexpr float CH_PROXVOICE_FLOOR         = 0.15f;

bool CHudCHProxVoice::Init()
{
	HOOK_MESSAGE(CHProxVoice);

	m_bActive = false;
	m_flRadius = 0.0f;
	m_bScaling = false;
	m_flBaseline = 1.0f;

	m_iFlags |= HUD_ACTIVE;

	gHUD.AddHudElem(this);
	return true;
}

bool CHudCHProxVoice::VidInit()
{
	return true;
}

// New server: whatever the last one said no longer applies. Not Reset() - that
// fires on every respawn, and this state outlives a respawn into the same round.
void CHudCHProxVoice::InitHUDData()
{
	RestoreBaseline();
	m_bActive = false;
}

bool CHudCHProxVoice::MsgFunc_CHProxVoice(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);
	m_bActive = READ_BYTE() != 0;
	m_flRadius = static_cast<float>(READ_SHORT());
	return true;
}

void CHudCHProxVoice::RestoreBaseline()
{
	if (!m_bScaling)
		return;

	m_bScaling = false;

	if (gEngfuncs.pVoiceTweak)
		gEngfuncs.pVoiceTweak->SetControlFloat(OtherSpeakerScale, m_flBaseline);
}

void CHudCHProxVoice::Think()
{
	IVoiceTweak* pTweak = gEngfuncs.pVoiceTweak;
	CVoiceStatus* pVoice = GetClientVoiceMgr();

	if (!m_bActive || m_flRadius <= 0.0f || !pTweak || !pVoice)
	{
		RestoreBaseline();
		return;
	}

	cl_entity_t* pLocal = gEngfuncs.GetLocalPlayer();
	if (!pLocal)
	{
		RestoreBaseline();
		return;
	}

	// Nearest player currently talking. A speaker the server has not updated
	// this frame is out of the PVS - at least a wall away - and their cached
	// origin is stale, so treat them as at the edge of the radius.
	float flNearest = -1.0f;
	const int localIndex = pLocal->index;

	for (int i = 1; i <= MAX_PLAYERS; i++)
	{
		if (i == localIndex || !pVoice->m_VoicePlayers[i - 1])
			continue;

		float flDist = m_flRadius;
		cl_entity_t* pClient = gEngfuncs.GetEntityByIndex(i);

		if (pClient && pClient->curstate.messagenum >= pLocal->curstate.messagenum)
			flDist = (pClient->origin - pLocal->origin).Length();

		if (flNearest < 0.0f || flDist < flNearest)
			flNearest = flDist;
	}

	// Nobody talking: hand the setting back, so an options-menu change made
	// between conversations is what the next one starts from.
	if (flNearest < 0.0f)
	{
		RestoreBaseline();
		return;
	}

	if (!m_bScaling)
	{
		m_bScaling = true;
		m_flBaseline = pTweak->GetControlFloat(OtherSpeakerScale);
	}

	const float flFullRange = m_flRadius * CH_PROXVOICE_FULL_FRACTION;
	float flScale = 1.0f;

	if (flNearest > flFullRange)
	{
		const float t = (flNearest - flFullRange) / (m_flRadius - flFullRange);
		flScale = 1.0f - (1.0f - CH_PROXVOICE_FLOOR) * V_min(t, 1.0f);
	}

	pTweak->SetControlFloat(OtherSpeakerScale, m_flBaseline * flScale);
}
