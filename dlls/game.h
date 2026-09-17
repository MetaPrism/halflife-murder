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

// Seconds into a live round before the Killer's "evil presence" starts to
// show: a slight dark tint over their own view, a notice telling them so, and
// a trail of black smoke behind them that everyone can see. A kill resets the
// clock and hides it again. 0 turns the mechanic off.
extern cvar_t ch_killerfogtime;

// Footsteps: every step, jump and landing leaves a print in the player's own
// colour that only the Killer can see (their own included), for this many
// seconds. 0 turns the mechanic off. See cl_dll/ch_footsteps.cpp for the drawing.
extern cvar_t ch_footsteps;

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

// Locked model: 1 puts every player in the "helmet" model for as long as it is
// on, whatever they picked and whether or not ch_anonymous is on. 0 leaves
// model choice alone.
extern cvar_t ch_lockmodel;

// Bare hands: how much damage a punch from weapon_hands does to a
// func_breakable, so anyone can get through a window instead of only the two
// armed players. Punches hurt nothing else. 0 turns punching off.
extern cvar_t ch_handsbreak;

// Disguise: 1 lets the Killer +use a corpse to take the dead player's name and
// colours (not their model) until their next kill or the round ends. Priced in
// loot by ch_disguise_cost; 0 there makes it free.
extern cvar_t ch_disguise;
extern cvar_t ch_disguise_cost;

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

// Password for "ch_adminpanel <pass>", the client-side Killer odds panel. Empty
// (the default) means only the listen server host can open it. The host never
// needs the password; anyone else needs it once per connection.
extern cvar_t ch_admin_pass;

// Bot behaviour: 1 leaves fake clients standing still (they only exist to fill
// out a round), 0 makes them wander in a straight line and pick a new direction
// whenever they run into something.
extern cvar_t bot_zombie;

// Which AI a moving bot runs, once bot_zombie is 0. 0 is that wander; 1 is the
// hunting bot in ch_bots.cpp, which traces its way past what is in front of it,
// crouches under it or crouch-jumps over it, and attacks players it meets with
// whatever its role was given.
extern cvar_t bot_hunt;

// How many bots the server keeps on its own. 0 (the default) turns the quota
// off and leaves "bot"/"bot_kickall" in full control. Otherwise ch_bots.cpp
// adds or kicks one bot a second until the count is right, and bot_quota_mode
// says what "right" means: "add" keeps exactly n bots regardless of who else
// is on, "fill" keeps n players total, so bots leave as humans join and come
// back as they go.
extern cvar_t bot_quota;
extern cvar_t bot_quota_mode;

// Register Crowbar Hunt's server console commands ("ch_odds"). Called once
// from GameDLLInit(), alongside the bot commands.
void InitCrowbarHuntCommands();

// Engine Cvars
inline cvar_t* g_psv_gravity;
inline cvar_t* g_psv_aim;
inline cvar_t* g_psv_allow_autoaim;
inline cvar_t* g_footsteps;
inline cvar_t* g_psv_cheats;
