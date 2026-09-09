#pragma once

// Dummy fake clients, for filling out a round while testing. They connect and
// occupy a player slot like anyone else, but never send input of their own.

// Registers the "bot" and "bot_kickall" server commands. Called once at DLL init.
void InitBotCommands();

// Keeps every fake client's physics running. Called once per server frame.
void BotThink();

// Forgets a bot's slot. Called for every client that leaves, bot or not.
void BotClientDisconnected(edict_t* pEntity);
