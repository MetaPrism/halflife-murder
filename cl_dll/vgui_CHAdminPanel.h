//
// vgui_CHAdminPanel.h
//
// Crowbar Hunt admin panel: the Killer draw odds table, one row per connected
// player, opened with "ch_adminpanel". The server decides who is an admin and
// only ever sends this table to them; the client's only job is to draw what
// arrives and ask for it again every so often while the window is up.
//

#pragma once

#include "vgui_TeamFortressViewport.h"

class CCHAdminPanel : public CMenuPanel
{
public:
	// One connected player's line in the table, as the server sent it.
	struct Row
	{
		int iSlot;                                // player index, for the anon colour lookup
		char szName[MAX_PLAYER_NAME_LENGTH];      // the name they joined with
		char szShownName[MAX_PLAYER_NAME_LENGTH]; // the name the room sees right now
		int iRole;                                // CHRole as sent: 0 unassigned, 1 spectator, 2 killer, 3 hunter, 4 survivor
		float flWeight;
		float flChance; // percent; 0 when not in the draw
		bool bInDraw;
		bool bDisguised;
	};

	CCHAdminPanel(int x, int y, int wide, int tall);

	// The server sends the table a row at a time - see gmsgCHAdmin. Rows land
	// in a pending copy so a half-arrived table never draws; EndTable() swaps
	// it in, records the cvars behind it, and repaints.
	void BeginTableIfNeeded();
	void AddRow(const Row& row);
	void EndTable(float flDecay, float flRecover);

	void Open() override;
	void paint() override;

private:
	// Repaint every label from m_Rows.
	void Refresh();

	static constexpr int MAX_ROWS = 32; // dlls/cdll_dll.h MAX_PLAYERS
	static constexpr float REFRESH_INTERVAL = 2.0f;

	Row m_Rows[MAX_ROWS];
	int m_iRowCount = 0;

	Row m_Pending[MAX_ROWS];
	int m_iPendingCount = 0;
	bool m_bReceiving = false;

	float m_flDecay = 0.0f;
	float m_flRecover = 0.0f;

	// gHUD.m_flTime at which paint() next re-sends "ch_adminpanel".
	float m_flNextRefresh = 0.0f;

	CTransparentPanel* m_pWindow = nullptr;
	vgui::Label* m_pTitle = nullptr;
	vgui::Label* m_pSubtitle = nullptr;
	static constexpr int NUM_COLS = 5; // player, shown as, role, weight, chance

	vgui::Label* m_pHeader[NUM_COLS] = {};
	vgui::ScrollPanel* m_pScroll = nullptr;
	vgui::Panel* m_pList = nullptr;
	vgui::Label* m_pCells[MAX_ROWS][NUM_COLS] = {};

	// The scheme's text colour, to put a row back to after it stops being the Killer's.
	int m_iTextColor[4] = {};
	vgui::Label* m_pEmpty = nullptr;
};
