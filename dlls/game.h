/***
*
*	Copyright (c) 1996-2001, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/

#pragma once

extern void GameDLLInit( void );
void GameDLLShutdown();


extern cvar_t	displaysoundlist;

// multiplayer server rules
extern cvar_t fragsleft;
extern cvar_t timeleft;
extern cvar_t teamplay;
extern cvar_t fraglimit;
extern cvar_t timelimit;
extern cvar_t friendlyfire;
extern cvar_t falldamage;
extern cvar_t weaponstay;
extern cvar_t forcerespawn;
extern cvar_t flashlight;
extern cvar_t aimcrosshair;
extern cvar_t decalfrequency;
extern cvar_t teamlist;
extern cvar_t teamoverride;
extern cvar_t defaultteam;
extern cvar_t allowmonsters;
extern cvar_t allow_spectators;
extern cvar_t mp_chattime;

extern cvar_t sv_allowbunnyhopping;

extern cvar_t sv_busters;

extern cvar_t sv_crowbarhunt;

// Seconds a thrown crowbar stays out of the Killer's hands before it returns to
// them by itself. 0 disables the return.
extern cvar_t ch_crowbar_return_time;

// Thickness of the kRenderFxGlowShell outline on a weapon lying in the world -
// red for the Killer's thrown crowbar, blue for a dropped revolver - so a gun
// on the floor reads as a gun from across the room. 0 turns the outlines off.
extern cvar_t ch_glow_shell;

// Ground speed the Killer gets while holding +sprint, and the speed every
// other player (and the Killer when not sprinting) is held to.
extern cvar_t ch_sprint_speed;
extern cvar_t ch_base_speed;

// Proximity voice chat: while a round is live, players only hear other players
// standing near them. 0 lets everyone hear everyone, as normal.
extern cvar_t ch_proxvoice;

// Seconds a revolver carrier is penalised for after shooting someone who wasn't
// the Killer: how long they stay slow, unable to jump properly, and barred from
// picking a revolver back up. 0 disables the punishment entirely.
extern cvar_t ch_punish_time;

// How dark the screen goes for the duration of that penalty, 0-255. 0 leaves
// the shooter's view alone and the rest of the penalty unchanged.
extern cvar_t ch_punish_tint;

// Seconds a round may run before it is called for the Survivors: the Killer
// has to finish the job inside this window. 0 lets a round run until someone
// wins outright.
extern cvar_t ch_round_time;

// Loot: how many seconds apart pieces appear during a live round (0 turns
// loot off), and how many may lie uncollected at once before spawning pauses.
// Where they appear is the map's business - see crowbar_hunt_loot.h.
extern cvar_t ch_loot_interval;
extern cvar_t ch_loot_max;

// Every this many pieces of loot collected in a round buys a revolver: handed
// over if the collector has none, dropped at their feet if they do. 0 makes
// loot worth nothing but the number.
extern cvar_t ch_loot_reward;

// 1 lets the Killer buy in too: one revolver, once a round, for twice the
// price. Off by default - a Killer who can shoot stops being the Killer.
extern cvar_t ch_loot_killer_gun;

// Anonymous mode: every round, each player is given a random colour and a
// random name off the names file, so nobody can be picked out of a round by
// the name or the colours they usually play under. Their model choice is left
// alone. 0 leaves everyone under their own name.
extern cvar_t ch_anonymous;

// How the Killer draw is weighted against repeats. Every player carries a
// weight, 1.0 by default, and the Killer is drawn in proportion to it. Whoever
// is drawn has their weight multiplied by ch_killer_decay; everyone else who
// was in the draw gets ch_killer_recover added back, up to 1.0. So a decay of
// 0.25 with a recover of 0.25 means a fresh Killer is a quarter as likely as
// anyone else next round and takes three rounds to come back to even.
// ch_killer_decay at 1 turns the whole thing off and restores a flat draw.
extern cvar_t ch_killer_decay;
extern cvar_t ch_killer_recover;

// Floor under a player's Killer weight, so a long streak of bad luck can never
// take somebody out of the draw entirely.
extern cvar_t ch_killer_min_weight;

// Bot behaviour: 1 leaves fake clients standing still (they only exist to fill
// out a round), 0 makes them wander in a straight line and pick a new direction
// whenever they run into something.
extern cvar_t bot_zombie;

// Register Crowbar Hunt's server console commands ("ch_odds"). Called once
// from GameDLLInit(), alongside the bot commands.
void InitCrowbarHuntCommands();

// Engine Cvars
inline cvar_t* g_psv_gravity;
inline cvar_t* g_psv_aim;
inline cvar_t* g_psv_allow_autoaim;
inline cvar_t* g_footsteps;
inline cvar_t* g_psv_cheats;
