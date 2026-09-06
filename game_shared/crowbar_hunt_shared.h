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
