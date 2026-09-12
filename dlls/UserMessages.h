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

inline int gmsgShake = 0;
inline int gmsgFade = 0;
inline int gmsgFlashlight = 0;
inline int gmsgFlashBattery = 0;
inline int gmsgResetHUD = 0;
inline int gmsgInitHUD = 0;
inline int gmsgShowGameTitle = 0;
inline int gmsgCurWeapon = 0;
inline int gmsgHealth = 0;
inline int gmsgDamage = 0;
inline int gmsgBattery = 0;
inline int gmsgTrain = 0;
inline int gmsgLogo = 0;
inline int gmsgWeaponList = 0;
inline int gmsgAmmoX = 0;
inline int gmsgHudText = 0;
inline int gmsgDeathMsg = 0;
inline int gmsgScoreInfo = 0;
inline int gmsgTeamInfo = 0;
inline int gmsgTeamScore = 0;
inline int gmsgGameMode = 0;
inline int gmsgMOTD = 0;
inline int gmsgServerName = 0;
inline int gmsgAmmoPickup = 0;
inline int gmsgWeapPickup = 0;
inline int gmsgItemPickup = 0;
inline int gmsgHideWeapon = 0;
inline int gmsgSetCurWeap = 0;
inline int gmsgSayText = 0;
inline int gmsgTextMsg = 0;
inline int gmsgSetFOV = 0;
inline int gmsgShowMenu = 0;
inline int gmsgGeigerRange = 0;
inline int gmsgTeamNames = 0;

inline int gmsgStatusText = 0;
inline int gmsgStatusValue = 0;

inline int gmsgWeapons = 0;

// Crowbar Hunt anonymous mode: which colour a player's name is drawn in, and
// the real name the scoreboard shows behind the disguise.
inline int gmsgCHAnon = 0;

// Crowbar Hunt round clock: seconds left in the round, 0 to take the clock
// off the HUD. The client counts down from there on its own.
inline int gmsgCHTimer = 0;

// Crowbar Hunt: how much loot the player has collected this round. Its
// arrival is also what swaps the HUD's armour readout for the loot one.
inline int gmsgCHLoot = 0;

// Crowbar Hunt: the local player's role for the top-of-HUD label. 0 takes the
// label down; the rest are the CHRole values Killer/Hunter/Survivor as 1/2/3.
inline int gmsgCHRole = 0;

// Crowbar Hunt: the map was just reset for a new round. Carries no data; the
// client strips its decals (blood, bullet holes, scorch marks) on receipt,
// since those live purely client-side and would otherwise outlast the round.
inline int gmsgCHClearFX = 0;

// Crowbar Hunt: which players are observers, so the scoreboard can file them
// under Spectators. The base SDK client already handles this message
// (MsgFunc_Spectator) but nothing server-side ever registered or sent it.
inline int gmsgSpectator = 0;

void LinkUserMessages();
