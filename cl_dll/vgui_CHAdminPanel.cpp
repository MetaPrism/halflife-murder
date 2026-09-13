//
// vgui_CHAdminPanel.cpp
//
// Crowbar Hunt admin panel - see vgui_CHAdminPanel.h.
//
// Built on the same pieces as the MOTD window: a shaded CMenuPanel filling the
// screen, a bordered CTransparentPanel for the window, scheme fonts, and
// CommandButtons along the bottom. The table itself is a grid of Labels rather
// than one TextPanel because the scheme fonts are proportional and a
// space-padded table does not line up in them.
//

#include <algorithm>

#include <VGUI_Font.h>
#include <VGUI_ScrollPanel.h>
#include <VGUI_LineBorder.h>

#include "hud.h"
#include "cl_util.h"
#include "const.h"

#include "vgui_int.h"
#include "vgui_TeamFortressViewport.h"
#include "vgui_CHAdminPanel.h"

#define ADMIN_WINDOW_X XRES(40)
#define ADMIN_WINDOW_Y YRES(56)
#define ADMIN_WINDOW_SIZE_X XRES(560)
#define ADMIN_WINDOW_SIZE_Y YRES(368)

#define ADMIN_MARGIN_X XRES(16)
#define ADMIN_TITLE_Y YRES(12)
#define ADMIN_SUBTITLE_Y YRES(36)
#define ADMIN_HEADER_Y YRES(60)
#define ADMIN_LIST_Y YRES(80)
#define ADMIN_ROW_H YRES(14)

// Column x offsets and widths, relative to the window's inner edge, in
// header/cell order: real name, name currently shown, role, weight, chance.
static const struct
{
	const char* pszHeader;
	int x, w;
	Label::Alignment align;
} g_AdminColumns[] = {
	{"Player", 0, 150, Label::a_west},
	{"Shown as", 156, 150, Label::a_west},
	{"Role", 312, 90, Label::a_west},
	{"Weight", 400, 50, Label::a_east},
	{"Chance", 456, 56, Label::a_east},
};

static const char* const g_pszAdminRoleNames[] = {"-", "Spectator", "Killer", "Hunter", "Survivor"};

// death.cpp: the colour a slot is wearing this round, 0x00RRGGBB, or -1 for none.
extern int GetCHAnonPackedColor(int clientIndex);

// The Refresh button: ask the server for the table again right now. The
// reply lands in the panel through the CHAdmin message like any other.
class CCHAdminRefreshHandler : public ActionSignal
{
public:
	void actionPerformed(Panel* panel) override
	{
		gEngfuncs.pfnClientCmd("ch_adminpanel\n");
	}
};

static Label* CreateCell(Panel* pParent, Font* pFont, int r, int g, int b, int a, int x, int y, int wide, int tall, Label::Alignment align)
{
	Label* pLabel = new Label("", x, y, wide, tall);
	pLabel->setParent(pParent);
	pLabel->setFont(pFont);
	pLabel->setFgColor(r, g, b, a);
	pLabel->setBgColor(0, 0, 0, 255); // fully transparent
	pLabel->setContentAlignment(align);
	pLabel->setVisible(true);
	return pLabel;
}

CCHAdminPanel::CCHAdminPanel(int x, int y, int wide, int tall) : CMenuPanel(100, false, x, y, wide, tall)
{
	CSchemeManager* pSchemes = gViewPort->GetSchemeManager();

	SchemeHandle_t hTitleScheme = pSchemes->getSchemeHandle("Title Font");
	SchemeHandle_t hTextScheme = pSchemes->getSchemeHandle("Scoreboard Text");
	SchemeHandle_t hSmallScheme = pSchemes->getSchemeHandle("Scoreboard Small Text");

	int r, g, b, a;

	m_pWindow = new CTransparentPanel(255, ADMIN_WINDOW_X, ADMIN_WINDOW_Y, ADMIN_WINDOW_SIZE_X, ADMIN_WINDOW_SIZE_Y);
	m_pWindow->setParent(this);
	m_pWindow->setBorder(new LineBorder(Color(255 * 0.7, 170 * 0.7, 0, 0)));
	m_pWindow->setVisible(true);

	int iXPos, iYPos, iXSize, iYSize;
	m_pWindow->getPos(iXPos, iYPos);
	m_pWindow->getSize(iXSize, iYSize);

	const int iInnerX = iXPos + ADMIN_MARGIN_X;
	const int iInnerW = iXSize - ADMIN_MARGIN_X * 2;

	// Title, the way the MOTD window does it: the scheme font and colour
	// first, then the App scheme's primary slots on top so hud_color applies.
	m_pTitle = new Label("", iInnerX, iYPos + ADMIN_TITLE_Y);
	m_pTitle->setParent(this);
	m_pTitle->setFont(pSchemes->getFont(hTitleScheme));
	m_pTitle->setFont(Scheme::sf_primary1);
	pSchemes->getFgColor(hTitleScheme, r, g, b, a);
	m_pTitle->setFgColor(r, g, b, a);
	m_pTitle->setFgColor(Scheme::sc_primary1);
	pSchemes->getBgColor(hTitleScheme, r, g, b, a);
	m_pTitle->setBgColor(r, g, b, a);
	m_pTitle->setContentAlignment(vgui::Label::a_west);
	m_pTitle->setText("Killer draw odds");

	pSchemes->getFgColor(hSmallScheme, r, g, b, a);
	m_pSubtitle = CreateCell(this, pSchemes->getFont(hSmallScheme), r, g, b, a,
		iInnerX, iYPos + ADMIN_SUBTITLE_Y, iInnerW, ADMIN_ROW_H, vgui::Label::a_west);
	m_pSubtitle->setText("Waiting for the server...");

	pSchemes->getFgColor(hTextScheme, r, g, b, a);
	Font* pTextFont = pSchemes->getFont(hTextScheme);

	m_iTextColor[0] = r;
	m_iTextColor[1] = g;
	m_iTextColor[2] = b;
	m_iTextColor[3] = a;

	for (int c = 0; c < NUM_COLS; c++)
	{
		m_pHeader[c] = CreateCell(this, pTextFont, r, g, b, a, iInnerX + XRES(g_AdminColumns[c].x), iYPos + ADMIN_HEADER_Y, XRES(g_AdminColumns[c].w), ADMIN_ROW_H, g_AdminColumns[c].align);
		m_pHeader[c]->setText(g_AdminColumns[c].pszHeader);
	}

	// The rows scroll; thirty-two of them do not fit under the header at
	// 640x480, and a scoreboard-style list is what an admin expects anyway.
	const int iListTall = iYSize - ADMIN_LIST_Y - YRES(24) - BUTTON_SIZE_Y;
	m_pScroll = new CTFScrollPanel(iInnerX, iYPos + ADMIN_LIST_Y, iInnerW, iListTall);
	m_pScroll->setParent(this);
	m_pScroll->setScrollBarAutoVisible(false, true);
	m_pScroll->setScrollBarVisible(false, false);

	m_pList = new Panel(0, 0, iInnerW, ADMIN_ROW_H * MAX_ROWS);
	m_pList->setParent(m_pScroll->getClient());
	m_pList->setPaintBackgroundEnabled(false);

	for (int i = 0; i < MAX_ROWS; i++)
	{
		const int iRowY = i * ADMIN_ROW_H;

		for (int c = 0; c < NUM_COLS; c++)
		{
			m_pCells[i][c] = CreateCell(m_pList, pTextFont, r, g, b, a, XRES(g_AdminColumns[c].x), iRowY, XRES(g_AdminColumns[c].w), ADMIN_ROW_H, g_AdminColumns[c].align);
			m_pCells[i][c]->setVisible(false);
		}
	}

	m_pEmpty = CreateCell(m_pList, pTextFont, r, g, b, a, 0, 0, iInnerW, ADMIN_ROW_H, vgui::Label::a_west);
	m_pEmpty->setText("Nobody connected");
	m_pEmpty->setVisible(false);

	m_pScroll->validate();

	// Buttons along the bottom, Refresh on the left and Close on the right.
	const int iButtonY = iYPos + iYSize - YRES(16) - BUTTON_SIZE_Y;

	CommandButton* pRefresh = new CommandButton("Refresh", iInnerX, iButtonY, CMENU_SIZE_X, BUTTON_SIZE_Y);
	pRefresh->addActionSignal(new CCHAdminRefreshHandler());
	pRefresh->setParent(this);

	CommandButton* pClose = new CommandButton("Close", iXPos + iXSize - ADMIN_MARGIN_X - CMENU_SIZE_X, iButtonY, CMENU_SIZE_X, BUTTON_SIZE_Y);
	pClose->addActionSignal(new CMenuHandler_TextWindow(HIDE_TEXTWINDOW));
	pClose->setParent(this);
}

void CCHAdminPanel::BeginTableIfNeeded()
{
	if (m_bReceiving)
		return;

	m_bReceiving = true;
	m_iPendingCount = 0;
}

void CCHAdminPanel::AddRow(const Row& row)
{
	BeginTableIfNeeded();

	if (m_iPendingCount >= MAX_ROWS)
		return;

	m_Pending[m_iPendingCount++] = row;
}

void CCHAdminPanel::EndTable(float flDecay, float flRecover)
{
	// A terminator with no rows before it is still a table: an empty one.
	BeginTableIfNeeded();

	m_iRowCount = m_iPendingCount;
	memcpy(m_Rows, m_Pending, sizeof(Row) * m_iRowCount);

	m_bReceiving = false;
	m_iPendingCount = 0;

	m_flDecay = flDecay;
	m_flRecover = flRecover;

	// Whoever is likeliest at the top, then whoever is sitting out, so the
	// answer to "who is probably next" needs no reading.
	std::sort(m_Rows, m_Rows + m_iRowCount, [](const Row& a, const Row& b) {
		if (a.bInDraw != b.bInDraw)
			return a.bInDraw;
		if (a.flChance != b.flChance)
			return a.flChance > b.flChance;
		if (a.flWeight != b.flWeight)
			return a.flWeight > b.flWeight;
		return stricmp(a.szName, b.szName) < 0;
	});

	Refresh();
}

void CCHAdminPanel::Refresh()
{
	char sz[64];

	if (m_flDecay >= 1.0f)
		m_pSubtitle->setText("Flat draw - ch_killer_decay is 1");
	else
	{
		sprintf(sz, "Weight x%.2f when drawn, +%.2f back per round", m_flDecay, m_flRecover);
		m_pSubtitle->setText(sz);
	}

	for (int i = 0; i < MAX_ROWS; i++)
	{
		const bool bShown = i < m_iRowCount;

		for (Label* pCell : m_pCells[i])
			pCell->setVisible(bShown);

		if (!bShown)
			continue;

		const Row& row = m_Rows[i];
		const bool bKiller = row.iRole == 2;

		// The Killer's line in red, so the one thing the admin most wants
		// from this table needs no reading at all.
		for (Label* pCell : m_pCells[i])
		{
			if (bKiller)
				pCell->setFgColor(255, 64, 64, 0);
			else
				pCell->setFgColor(m_iTextColor[0], m_iTextColor[1], m_iTextColor[2], m_iTextColor[3]);
		}

		m_pCells[i][0]->setText("%s", row.szName);

		// Only worth a column when it differs: outside anonymous mode every
		// line would just repeat itself. Drawn in the colour the round dealt
		// them - the same table the scoreboard and chat use, so "Alpha" here
		// looks like Alpha out there.
		if (strcmp(row.szName, row.szShownName) != 0)
		{
			m_pCells[i][1]->setText("%s", row.szShownName);

			const int packed = GetCHAnonPackedColor(row.iSlot);

			if (packed >= 0)
				m_pCells[i][1]->setFgColor((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF, 0);
		}
		else
			m_pCells[i][1]->setText("");

		const int iRole = (row.iRole >= 0 && row.iRole < (int)(sizeof(g_pszAdminRoleNames) / sizeof(g_pszAdminRoleNames[0]))) ? row.iRole : 0;
		sprintf(sz, "%s%s", g_pszAdminRoleNames[iRole], row.bDisguised ? " (disguised)" : "");
		m_pCells[i][2]->setText(sz);

		sprintf(sz, "%.2f", row.flWeight);
		m_pCells[i][3]->setText(sz);

		if (row.bInDraw)
			sprintf(sz, "%.1f%%", row.flChance);
		else
			strcpy(sz, "out");
		m_pCells[i][4]->setText(sz);
	}

	m_pEmpty->setVisible(m_iRowCount == 0);

	// Size the list to the rows so the scrollbar only appears when needed.
	int iWide, iTall;
	m_pList->getSize(iWide, iTall);
	m_pList->setSize(iWide, ADMIN_ROW_H * V_max(1, m_iRowCount));
	m_pScroll->validate();

	repaint();
}

void CCHAdminPanel::Open()
{
	CMenuPanel::Open();

	// The table that opened us is fresh; the first re-request can wait.
	m_flNextRefresh = gHUD.m_flTime + REFRESH_INTERVAL;
}

// Odds move whenever somebody dies or joins, and a panel that sat on stale
// numbers would be worse than none. Re-ask while open; the server answers
// with a full table each time, which the CHAdmin handler feeds back here.
void CCHAdminPanel::paint()
{
	CMenuPanel::paint();

	if (!isVisible())
		return;

	if (gHUD.m_flTime >= m_flNextRefresh)
	{
		m_flNextRefresh = gHUD.m_flTime + REFRESH_INTERVAL;
		gEngfuncs.pfnClientCmd("ch_adminpanel\n");
	}
}
