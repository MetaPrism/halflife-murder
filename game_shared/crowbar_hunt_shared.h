/***
*
*	Crowbar Hunt: definitions shared between the server and client DLLs.
*
****/

#pragma once

// Marks a ch_corpse: a body left behind by a player who is now an observer.
//
// The client's studio renderer keys off this to draw the corpse as the player
// it came from - the model and the colours a plain studio entity can never
// reach, because SetupPlayerModel() and StudioSetRemapColors() are only wired
// up on the player draw path. renderfx is networked as 8 unsigned bits
// (network/delta.lst), and the engine's own kRenderFx* values stop at 21, so a
// value up here is ours alone and nothing in the engine will act on it.
//
// The player's slot travels in renderamt, which is also 8 bits - enough for
// any client index.
#define kRenderFxCHCorpse 64

// --- Anonymous mode (ch_anonymous) ------------------------------------------
//
// One entry from this table is dealt to each player at the start of every
// round. The hue goes into their userinfo topcolor/bottomcolor, so the studio
// renderer remaps their model to it, and the same entry's rgb is what the
// client draws their name in - one colour is the whole of a player's identity
// for the round, worn and written.
//
// hue is what StudioSetRemapColors() ends up taking. It is NOT degrees: the
// engine spreads 0..255 over the whole 360 degree wheel, so a hue of 255 comes
// back around to red rather than stopping at violet. Getting that backwards is
// what made the first version of this table drift - the warm end looked right
// and everything from cyan on was a colour or two out. The comment beside each
// entry is the real angle it lands on.
//
// One byte therefore covers the entire wheel, which is what lets a ch_corpse
// match its owner: a corpse carries its colours in pev->colormap, one byte per
// component, so nothing here can be out of a body's reach.
//
// rgb is that same angle at full saturation and value, in the 0..1 floats
// GetClientColor() hands back. Not const: the client returns a pointer
// straight into this table, and callers hold it across frames.
struct CHAnonColor
{
	int   hue;
	float rgb[3];
};

inline CHAnonColor g_CHAnonColors[] = {
	{  0, {1.00f, 0.00f, 0.00f}}, // 0deg   red
	{ 26, {1.00f, 0.60f, 0.00f}}, // 36deg  orange
	{ 51, {0.80f, 1.00f, 0.00f}}, // 72deg  chartreuse
	{ 77, {0.20f, 1.00f, 0.00f}}, // 108deg green
	{102, {0.00f, 1.00f, 0.40f}}, // 144deg spring
	{128, {0.00f, 1.00f, 1.00f}}, // 180deg cyan
	{153, {0.00f, 0.40f, 1.00f}}, // 216deg azure
	{179, {0.20f, 0.00f, 1.00f}}, // 252deg violet
	{204, {0.80f, 0.00f, 1.00f}}, // 288deg purple
	{230, {1.00f, 0.00f, 0.60f}}, // 324deg pink
};

constexpr int CH_NUM_ANON_COLORS = sizeof(g_CHAnonColors) / sizeof(g_CHAnonColors[0]);

// Sent in gmsgCHAnon in place of a colour index to say "no anonymous identity"
// - the cvar is off, or this player joined after the round's deal.
#define CH_ANON_NONE 255

// Longest anonymous name including the terminator; the engine truncates player
// names to MAX_PLAYER_NAME_LENGTH (32) anyway.
#define CH_MAX_ANON_NAME 32

// Value sent in gmsgGameMode by CHalfLifeCrowbarHunt::UpdateGameMode(). The
// base SDK sends 0 for deathmatch and 1 for teamplay; the client's scoreboard
// keys its Players/Spectators layout off this one.
#define CH_GAMEMODE_CROWBARHUNT 2
