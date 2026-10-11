#include "PCH.h"
#include "Settings.h"

namespace
{
	std::filesystem::path IniPath()
	{
		return std::filesystem::absolute(std::filesystem::path{ "Data/SKSE/Plugins/" ANIMUNCULI_NAME ".ini" });
	}

	std::string ReadString(const std::wstring& a_file, const wchar_t* a_section, const wchar_t* a_key, const std::string& a_default)
	{
		wchar_t buf[1024]{};
		const std::wstring def{ a_default.begin(), a_default.end() };
		GetPrivateProfileStringW(a_section, a_key, def.c_str(), buf, static_cast<DWORD>(std::size(buf)), a_file.c_str());
		std::wstring w{ buf };
		// strip inline comments ("value ; comment") and whitespace
		if (const auto pos = w.find_first_of(L";#"); pos != std::wstring::npos) {
			w.resize(pos);
		}
		while (!w.empty() && iswspace(w.back())) {
			w.pop_back();
		}
		while (!w.empty() && iswspace(w.front())) {
			w.erase(w.begin());
		}
		const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<std::size_t>(len), '\0');
		WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
		return out;
	}

	bool ReadBool(const std::wstring& f, const wchar_t* s, const wchar_t* k, bool d)
	{
		auto v = ReadString(f, s, k, d ? "true" : "false");
		std::ranges::transform(v, v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return v == "1" || v == "true" || v == "yes" || v == "on";
	}

	float ReadFloat(const std::wstring& f, const wchar_t* s, const wchar_t* k, float d)
	{
		try {
			return std::stof(ReadString(f, s, k, std::to_string(d)));
		} catch (...) {
			return d;
		}
	}

	long long ReadInt(const std::wstring& f, const wchar_t* s, const wchar_t* k, long long d)
	{
		try {
			return std::stoll(ReadString(f, s, k, std::to_string(d)), nullptr, 0);  // accepts 0x..
		} catch (...) {
			return d;
		}
	}
}

Settings& Settings::Mutable()
{
	static Settings s;
	return s;
}

void Settings::Load()
{
	auto&       s = Mutable();
	const auto  path = IniPath();
	const auto  f = path.wstring();
	const bool  exists = std::filesystem::exists(path);

	s.serverEnabled = ReadBool(f, L"Server", L"Enabled", s.serverEnabled);
	s.bindAddress = ReadString(f, L"Server", L"BindAddress", s.bindAddress);
	s.port = static_cast<int>(ReadInt(f, L"Server", L"Port", s.port));
	s.token = ReadString(f, L"Server", L"Token", s.token);

	s.defaultRadiusM = ReadFloat(f, L"Perception", L"DefaultRadiusMeters", s.defaultRadiusM);
	s.maxRadiusM = ReadFloat(f, L"Perception", L"MaxRadiusMeters", s.maxRadiusM);
	s.requireLineOfSight = ReadBool(f, L"Perception", L"RequireLineOfSight", s.requireLineOfSight);

	s.voiceEnabled = ReadBool(f, L"Voice", L"Enabled", s.voiceEnabled);
	s.volume = std::clamp(ReadFloat(f, L"Voice", L"Volume", s.volume), 0.f, 4.f);
	s.followMasterVolume = ReadBool(f, L"Voice", L"FollowMasterVolume", s.followMasterVolume);
	s.subtitleMode = ReadString(f, L"Voice", L"SubtitleMode", s.subtitleMode);
	s.lipSync = ReadBool(f, L"Voice", L"LipSync", s.lipSync);
	s.lipSyncGain = std::clamp(ReadFloat(f, L"Voice", L"LipSyncGain", s.lipSyncGain), 0.f, 3.f);
	s.fullVolumeDistanceM = std::max(0.1f, ReadFloat(f, L"Voice", L"FullVolumeDistanceMeters", s.fullVolumeDistanceM));
	s.silentDistanceM = std::max(s.fullVolumeDistanceM + 0.1f, ReadFloat(f, L"Voice", L"SilentDistanceMeters", s.silentDistanceM));

	s.visionEnabled = ReadBool(f, L"Vision", L"Enabled", s.visionEnabled);
	s.allowAgentPerspective = ReadBool(f, L"Vision", L"AllowAgentPerspective", s.allowAgentPerspective);
	s.keepsakeFolder = ReadString(f, L"Vision", L"KeepsakeFolder", s.keepsakeFolder);
	s.maxDimension = std::clamp(static_cast<int>(ReadInt(f, L"Vision", L"MaxDimension", s.maxDimension)), 64, 4096);

	s.actionsEnabled = ReadBool(f, L"Actions", L"Enabled", s.actionsEnabled);
	s.allowAttack = ReadBool(f, L"Actions", L"AllowAttack", s.allowAttack);
	s.allowAttackPlayer = ReadBool(f, L"Actions", L"AllowAttackPlayer", s.allowAttackPlayer);
	s.allowReadPlayerInventory = ReadBool(f, L"Actions", L"AllowReadPlayerInventory", s.allowReadPlayerInventory);

	s.chatEnabled = ReadBool(f, L"Chat", L"Enabled", s.chatEnabled);
	s.chatHotkey = static_cast<std::uint32_t>(ReadInt(f, L"Chat", L"Hotkey", s.chatHotkey));
	s.chatPausesGame = ReadBool(f, L"Chat", L"PauseGame", s.chatPausesGame);

	s.allowConsole = ReadBool(f, L"Debug", L"AllowConsole", s.allowConsole);
	s.logLevel = ReadString(f, L"Debug", L"LogLevel", s.logLevel);

	logger::info("Settings {} ({}): server {}:{} enabled={}, voice={}, vision={} (agent POV {}), actions={}, chat={} (key {:#x}), console={}",
		exists ? "loaded" : "defaulted", path.string(), s.bindAddress, s.port, s.serverEnabled, s.voiceEnabled,
		s.visionEnabled, s.allowAgentPerspective, s.actionsEnabled, s.chatEnabled, s.chatHotkey, s.allowConsole);
}
