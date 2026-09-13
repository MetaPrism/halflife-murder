//
// ch_footsteps.cpp
//
// Crowbar Hunt footprints, as the Killer sees them. The server reports each
// footfall, jump and landing as one CHFootstep message - where, which way it
// faces, what colour, how long it lasts - and this keeps the lot and paints
// them on the floor. Only the Killer is ever sent any, so on every other
// client this holds nothing and draws nothing.
//
// Drawn through the triangle API rather than as sprite entities: a full server
// walking for three minutes is thousands of prints, and the engine's visible
// entity list would run out long before that. A quad per print costs nothing
// to speak of, and there is no limit to trip. Only prints within
// CH_FOOTSTEP_VIEW_DISTANCE of the eye are drawn - the point of a trail is
// what is near you - and the newest are preferred when there are more than
// CH_FOOTSTEP_MAX_DRAWN in reach.
//
// The sprite: one .spr with two frames, frame 0 the left foot and frame 1 the
// right. A single-frame sprite works too - it is mirrored for the right foot.
// Toe at the top of the image. Drawn additive, so paint the foot white on a
// black background and the colour comes from the message.
//

#include "hud.h"
#include "cl_util.h"
#include "parsemsg.h"
#include "com_model.h"
#include "triangleapi.h"

DECLARE_MESSAGE(m_CHFootsteps, CHFootstep)

extern Vector v_origin;

constexpr const char* CH_FOOTSTEP_SPRITE = "sprites/ch_footstep.spr";

// Prints fully in view out to CH_FOOTSTEP_FADE_DISTANCE, fading to nothing at
// CH_FOOTSTEP_VIEW_DISTANCE so the edge of what the Killer can see is soft.
constexpr float CH_FOOTSTEP_VIEW_DISTANCE = 800.0f;
constexpr float CH_FOOTSTEP_FADE_DISTANCE = 600.0f;

// The last CH_FOOTSTEP_FADE_TIME seconds of a print's life fade it out, and
// a fresh one takes CH_FOOTSTEP_FADE_IN seconds to arrive rather than pop.
constexpr float CH_FOOTSTEP_FADE_TIME = 8.0f;
constexpr float CH_FOOTSTEP_FADE_IN   = 0.25f;

// Size of the quad a print is drawn on, in world units, and its brightness
// at full life and range. A foot is about a dozen units long.
constexpr float CH_FOOTSTEP_SIZE  = 14.0f;
constexpr float CH_FOOTSTEP_ALPHA = 0.85f;

constexpr int CH_FOOTSTEP_RENDERMODE = kRenderTransAdd;

bool CHudCHFootsteps::Init()
{
	HOOK_MESSAGE(CHFootstep);

	m_hSprite = 0;
	Clear();

	// Never HUD_ACTIVE: nothing to draw in the 2D pass. The prints go down in
	// DrawTriangles(), which the engine's transparent triangle pass calls.
	gHUD.AddHudElem(this);
	return true;
}

bool CHudCHFootsteps::VidInit()
{
	m_hSprite = 0;

	// Only if it is there to load: a client without the sprite (an old
	// install, or the file not made yet) should just not see prints.
	int iLength = 0;

	if (byte* pFile = gEngfuncs.COM_LoadFile(CH_FOOTSTEP_SPRITE, 5, &iLength); pFile != nullptr)
	{
		gEngfuncs.COM_FreeFile(pFile);
		m_hSprite = SPR_Load(CH_FOOTSTEP_SPRITE);
	}

	return true;
}

// New server: whatever the last one showed us no longer applies.
void CHudCHFootsteps::InitHUDData()
{
	Clear();
}

void CHudCHFootsteps::Clear()
{
	m_iCount = 0;
	m_iNext = 0;
}

bool CHudCHFootsteps::MsgFunc_CHFootstep(const char* pszName, int iSize, void* pbuf)
{
	BEGIN_READ(pbuf, iSize);

	const int iKind = READ_BYTE();

	if (iKind == 0)
	{
		Clear();
		return true;
	}

	CHFootprint& print = m_prints[m_iNext];

	print.origin.x = READ_COORD();
	print.origin.y = READ_COORD();
	print.origin.z = READ_COORD();
	print.yaw = READ_ANGLE();
	print.rgb[0] = READ_BYTE() / 255.0f;
	print.rgb[1] = READ_BYTE() / 255.0f;
	print.rgb[2] = READ_BYTE() / 255.0f;

	const float flLife = static_cast<float>(READ_SHORT());

	print.bRight = iKind == 2;
	print.flBorn = gHUD.m_flTime;
	print.flDies = gHUD.m_flTime + flLife;

	// A ring: once full, the oldest print makes way for the newest.
	m_iNext = (m_iNext + 1) % CH_MAX_FOOTPRINTS;

	if (m_iCount < CH_MAX_FOOTPRINTS)
		m_iCount++;

	return true;
}

// From HUD_DrawTransparentTriangles(), once per frame while a level is up.
void CHudCHFootsteps::DrawTriangles()
{
	if (m_iCount == 0)
		return;

	// Drop what has run out. The ring is in age order from the oldest slot
	// on, so everything to expire is at the front of it.
	const float flNow = gHUD.m_flTime;

	while (m_iCount > 0)
	{
		const int iOldest = (m_iNext - m_iCount + CH_MAX_FOOTPRINTS) % CH_MAX_FOOTPRINTS;

		if (m_prints[iOldest].flDies > flNow)
			break;

		m_iCount--;
	}

	if (m_iCount == 0 || 0 == m_hSprite)
		return;

	model_t* pModel = const_cast<model_t*>(gEngfuncs.GetSpritePointer(m_hSprite));

	if (!pModel)
		return;

	// Both feet from one frame, if that is all the sprite has.
	const bool bMirrorRight = pModel->numframes < 2;

	triangleapi_t* pTri = gEngfuncs.pTriAPI;

	pTri->RenderMode(CH_FOOTSTEP_RENDERMODE);
	pTri->CullFace(TRI_NONE);

	const float flHalf = CH_FOOTSTEP_SIZE * 0.5f;

	int iDrawn = 0;

	// Newest first, so the cap keeps the freshest trail when the floor is
	// crowded.
	for (int n = 1; n <= m_iCount && iDrawn < CH_FOOTSTEP_MAX_DRAWN; n++)
	{
		const CHFootprint& print = m_prints[(m_iNext - n + CH_MAX_FOOTPRINTS) % CH_MAX_FOOTPRINTS];

		const float flDistance = (print.origin - v_origin).Length();

		if (flDistance > CH_FOOTSTEP_VIEW_DISTANCE)
			continue;

		float flAlpha = CH_FOOTSTEP_ALPHA;

		if (flDistance > CH_FOOTSTEP_FADE_DISTANCE)
			flAlpha *= (CH_FOOTSTEP_VIEW_DISTANCE - flDistance) / (CH_FOOTSTEP_VIEW_DISTANCE - CH_FOOTSTEP_FADE_DISTANCE);

		const float flLeft = print.flDies - flNow;

		if (flLeft < CH_FOOTSTEP_FADE_TIME)
			flAlpha *= flLeft / CH_FOOTSTEP_FADE_TIME;

		const float flAge = flNow - print.flBorn;

		if (flAge < CH_FOOTSTEP_FADE_IN)
			flAlpha *= flAge / CH_FOOTSTEP_FADE_IN;

		if (flAlpha <= 0.0f)
			continue;

		// The quad lies flat, its top edge toward the yaw the player was
		// facing. right is yaw swung 90 degrees clockwise, seen from above.
		const float flYaw = print.yaw * (M_PI / 180.0f);
		const Vector vecForward(cosf(flYaw), sinf(flYaw), 0.0f);
		const Vector vecRight(sinf(flYaw), -cosf(flYaw), 0.0f);

		const Vector vecTop = print.origin + vecForward * flHalf;
		const Vector vecBottom = print.origin - vecForward * flHalf;

		const float u0 = (print.bRight && bMirrorRight) ? 1.0f : 0.0f;
		const float u1 = 1.0f - u0;

		pTri->SpriteTexture(pModel, (print.bRight && !bMirrorRight) ? 1 : 0);
		pTri->Color4f(print.rgb[0], print.rgb[1], print.rgb[2], flAlpha);

		pTri->Begin(TRI_QUADS);

		pTri->TexCoord2f(u0, 0.0f);
		pTri->Vertex3fv(vecTop - vecRight * flHalf);

		pTri->TexCoord2f(u1, 0.0f);
		pTri->Vertex3fv(vecTop + vecRight * flHalf);

		pTri->TexCoord2f(u1, 1.0f);
		pTri->Vertex3fv(vecBottom + vecRight * flHalf);

		pTri->TexCoord2f(u0, 1.0f);
		pTri->Vertex3fv(vecBottom - vecRight * flHalf);

		pTri->End();

		iDrawn++;
	}

	pTri->RenderMode(kRenderNormal);
}
