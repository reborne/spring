#include "SelectMenu.h"

#include <SDL_keysym.h>
#include <SDL_timer.h>
#include <boost/bind.hpp>
#include <sstream>
#ifndef _WIN32
	#include <unistd.h>
	#define EXECLP execlp
#else
	#include <process.h>
	#define EXECLP _execlp
#endif
#include <stack>
#include <boost/cstdint.hpp>

#include "UpdaterWindow.h"
#include "ClientSetup.h"
#include "SelectionWidget.h"
#include "PreGame.h"
#include "Rendering/glFont.h"
#include "LogOutput.h"
#include "Exceptions.h"
#include "TdfParser.h"
#include "Util.h"
#include "FileSystem/ArchiveScanner.h"
#include "FileSystem/FileHandler.h"
#include "FileSystem/VFSHandler.h"
#include "FileSystem/FileSystem.h"
#include "ConfigHandler.h"
#include "InputHandler.h"
#include "StartScripts/ScriptHandler.h"
#include "StartScripts/SkirmishAITestScript.h"
#include "aGui/Gui.h"
#include "aGui/VerticalLayout.h"
#include "aGui/HorizontalLayout.h"
#include "aGui/Button.h"
#include "aGui/LineEdit.h"
#include "aGui/TextElement.h"
#include "aGui/Window.h"
#include "aGui/Picture.h"

using std::string;
using agui::Button;
using agui::HorizontalLayout;

extern boost::uint8_t* keys;
extern bool globalQuit;

class ConnectWindow : public agui::Window
{
public:
	ConnectWindow() : agui::Window("Connect to server")
	{
		agui::gui->AddElement(this);
		SetPos(0.5, 0.5);
		SetSize(0.4, 0.2);
		
		agui::VerticalLayout* wndLayout = new agui::VerticalLayout(this);
		HorizontalLayout* input = new HorizontalLayout(wndLayout);
		agui::TextElement* label = new agui::TextElement("Address:", input);
		address = new agui::LineEdit(input);
		address->SetFocus(true);
		address->SetContent(configHandler->GetString("address", ""));
		HorizontalLayout* buttons = new HorizontalLayout(wndLayout);
		Button* close = new Button("Close", buttons);
		close->Clicked.connect(boost::bind(&ConnectWindow::Finish, this, false));
		Button* connect = new Button("Connect", buttons);
		connect->Clicked.connect(boost::bind(&ConnectWindow::Finish, this, true));
		GeometryChange();
	}

	boost::signal<void (std::string)> Connect;
	agui::LineEdit* address;
	
private:
	void Finish(bool connect)
	{
		if (connect)
			Connect(address->GetContent());
		else
			WantClose();
	};
};

std::string CreateDefaultSetup(const std::string& map, const std::string& mod, const std::string& script,
			const std::string& playername)
{
	TdfParser::TdfSection setup;
	TdfParser::TdfSection* game = setup.construct_subsection("GAME");
	game->add_name_value("Mapname", map);
	game->add_name_value("Gametype", mod);

	TdfParser::TdfSection* modopts = game->construct_subsection("MODOPTIONS");
	modopts->AddPair("GameMode", 3);
	modopts->AddPair("MaxSpeed", 20);
	modopts->add_name_value("Scriptname", script);

	game->AddPair("IsHost", 1);
	game->add_name_value("MyPlayerName", playername);

	game->AddPair("NoHelperAIs", configHandler->Get("NoHelperAIs", 0));

	TdfParser::TdfSection* player0 = game->construct_subsection("PLAYER0");
	player0->add_name_value("Name", playername);
	player0->AddPair("Team", 0);

	const bool isSkirmishAITestScript =
			(script.substr(0, CSkirmishAITestScript::SCRIPT_NAME_PRELUDE.size())
			== CSkirmishAITestScript::SCRIPT_NAME_PRELUDE);
	if (!isSkirmishAITestScript) {
		TdfParser::TdfSection* player1 = game->construct_subsection("PLAYER1");
		player1->add_name_value("Name", "Enemy");
		player1->AddPair("Team", 1);
	}

	TdfParser::TdfSection* team0 = game->construct_subsection("TEAM0");
	team0->AddPair("TeamLeader", 0);
	team0->AddPair("AllyTeam", 0);

	TdfParser::TdfSection* team1 = game->construct_subsection("TEAM1");
	if (isSkirmishAITestScript) {
		team1->AddPair("TeamLeader", 0);
	} else {
		team1->AddPair("TeamLeader", 1);
	}
	team1->AddPair("AllyTeam", 1);

	TdfParser::TdfSection* ally0 = game->construct_subsection("ALLYTEAM0");
	ally0->AddPair("NumAllies", 0);

	TdfParser::TdfSection* ally1 = game->construct_subsection("ALLYTEAM1");
	ally1->AddPair("NumAllies", 0);

	std::ostringstream str;
	setup.print(str);

	return str.str();
}

SelectMenu::SelectMenu(bool server) : GuiElement(NULL), conWindow(NULL), updWindow(NULL)
{
	SetPos(0,0);
	SetSize(1,1);
	agui::gui->AddElement(this, true);
	mySettings = new ClientSetup();

	mySettings->isHost = server;
	mySettings->myPlayerName = configHandler->GetString("name", "UnnamedPlayer");
	if (mySettings->myPlayerName.empty()) {
		mySettings->myPlayerName = "UnnamedPlayer";
	} else {
		mySettings->myPlayerName = StringReplaceInPlace(mySettings->myPlayerName, ' ', '_');
	}

	{ // GUI stuff
		agui::Picture* background = new agui::Picture(this);;
		{
			std::string archive = archiveScanner->ModNameToModArchive("Spring Bitmaps");
			std::string archivePath = archiveScanner->GetArchivePath(archive)+archive;
			vfsHandler->AddArchive(archivePath, false);
			background->Load("bitmaps/ui/background.jpg");
			vfsHandler->RemoveArchive(archivePath);
		}
		selw = new SelectionWidget(this);
		agui::VerticalLayout* menu = new agui::VerticalLayout(this);
		menu->SetPos(0.1, 0.5);
		menu->SetSize(0.4, 0.4);
		menu->SetBorder(1.2f);
		agui::TextElement* name = new agui::TextElement("Spring", menu);
		Button* single = new Button("Test the Game", menu);
		single->Clicked.connect(boost::bind(&SelectMenu::Single, this));
		Button* multi = new Button("Start the Lobby", menu);
		multi->Clicked.connect(boost::bind(&SelectMenu::Multi, this));
		Button* update = new Button("Check for updates", menu);
		update->Clicked.connect(boost::bind(&SelectMenu::ShowUpdateWindow, this, true));
		Button* settings = new Button("Edit settings", menu);
		settings->Clicked.connect(boost::bind(&SelectMenu::Settings, this));
		Button* direct = new Button("Direct connect", menu);
		direct->Clicked.connect(boost::bind(&SelectMenu::ShowConnectWindow, this, true));
		Button* quit = new Button("Quit", menu);
		quit->Clicked.connect(boost::bind(&SelectMenu::Quit, this));
		background->GeometryChange();
	}

	if (!mySettings->isHost) {
		ShowConnectWindow(true);
	}
}

SelectMenu::~SelectMenu()
{
	ShowConnectWindow(false);
}

bool SelectMenu::Draw()
{
	SDL_Delay(10); // milliseconds
	ClearScreen();
	agui::gui->Draw();

	return true;
}

bool SelectMenu::Update()
{
	if (updWindow)
		updWindow->Poll();
	return true;
}

void SelectMenu::Single()
{
	static bool once = false;
	if (selw->userMod == SelectionWidget::NoModSelect)
	{
		selw->ShowModList();
	}
	else if (selw->userMap == SelectionWidget::NoMapSelect)
	{
		selw->ShowMapList();
	}
	else if (selw->userScript == SelectionWidget::NoScriptSelect)
	{
		selw->ShowScriptList();
	}
	else if (!once) // in case of double-click
	{
		once = true;
		mySettings->isHost = true;
		pregame = new CPreGame(mySettings);
		pregame->LoadSetupscript(CreateDefaultSetup(selw->userMap, selw->userMod, selw->userScript, mySettings->myPlayerName));
		agui::gui->RmElement(this);
	}
}

void SelectMenu::Settings()
{
#ifdef __unix__
	const std::string settingsProgram = "springsettings";
#else
	const std::string settingsProgram = "springsettings.exe";
#endif
	EXECLP(settingsProgram.c_str(), settingsProgram.c_str(), NULL);
}

void SelectMenu::Multi()
{
#ifdef __unix__
	const std::string defLobby = configHandler->GetString("DefaultLobby", "springlobby");
#else
	const std::string defLobby = configHandler->GetString("DefaultLobby", "springlobby.exe");
#endif
	EXECLP(defLobby.c_str(), defLobby.c_str(), NULL);
}

void SelectMenu::Quit()
{
	globalQuit = true;
}

void SelectMenu::ShowConnectWindow(bool show)
{
	if (show && !conWindow)
	{
		conWindow = new ConnectWindow();
		conWindow->Connect.connect(boost::bind(&SelectMenu::DirectConnect, this, _1));
		conWindow->WantClose.connect(boost::bind(&SelectMenu::ShowConnectWindow, this, false));
	}
	else if (!show && conWindow)
	{
		agui::gui->RmElement(conWindow);
		conWindow = NULL;
	}
}

void SelectMenu::ShowUpdateWindow(bool show)
{
	if (show && !updWindow)
	{
		updWindow = new UpdaterWindow();
		updWindow->WantClose.connect(boost::bind(&SelectMenu::ShowUpdateWindow, this, false));
	}
	else if (!show && updWindow)
	{
		agui::gui->RmElement(updWindow);
		updWindow = NULL;
	}
}

void SelectMenu::DirectConnect(const std::string& addr)
{
	configHandler->SetString("address", addr);
	mySettings->hostip = addr;
	mySettings->isHost = false;
	pregame = new CPreGame(mySettings);
	agui::gui->RmElement(this);
}

bool SelectMenu::HandleEventSelf(const SDL_Event& ev)
{
	switch (ev.type) {
		case SDL_KEYDOWN: {
			if (ev.key.keysym.sym == SDLK_ESCAPE)
			{
				if(keys[SDLK_LSHIFT])
				{
					logOutput.Print("User exited");
					globalQuit=true;
				}
				else
				{
					logOutput.Print("Use shift-esc to quit");
				}
			}
			else if (ev.key.keysym.sym == SDLK_RETURN)
			{
				Single();
				return true;
			}
			break;
		}
	}
	return false;
}
