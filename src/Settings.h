#pragma once

// Data/SKSE/Plugins/Animunculi.ini. Read once at load; every key has a safe default.
struct Settings
{
	// [Server]
	bool        serverEnabled{ true };
	std::string bindAddress{ "127.0.0.1" };
	int         port{ 47700 };
	std::string token;  // empty -> random per session, published via the discovery file

	// [Perception]
	float defaultRadiusM{ 40.f };
	float maxRadiusM{ 150.f };
	bool  requireLineOfSight{ false };

	// [Voice]
	bool        voiceEnabled{ true };
	float       volume{ 1.f };
	bool        followMasterVolume{ true };
	std::string subtitleMode{ "subtitle" };  // subtitle | notification | none
	bool        lipSync{ true };
	float       lipSyncGain{ 1.f };
	float       fullVolumeDistanceM{ 4.f };
	float       silentDistanceM{ 45.f };

	// [Vision]
	bool visionEnabled{ true };
	bool allowAgentPerspective{ true };
	int  maxDimension{ 1568 };
	std::string keepsakeFolder;  // empty -> Documents/My Games/Skyrim Special Edition/Screenshots/Animunculi

	// [Actions]
	bool actionsEnabled{ true };
	bool allowAttack{ true };
	bool allowAttackPlayer{ false };
	bool allowReadPlayerInventory{ true };

	// [Chat]
	bool         chatEnabled{ true };
	std::uint32_t chatHotkey{ 0x2B };  // DirectInput scan code; 0x2B = backslash
	bool         chatPausesGame{ true };

	// [Debug]
	bool        allowConsole{ false };
	std::string logLevel{ "info" };

	static Settings&       Mutable();
	static const Settings& Get() { return Mutable(); }
	static void            Load();
};
