#include "PCH.h"

#include "Settings.h"
#include "core/Text.h"
#include "game/Actions.h"
#include "game/Agents.h"
#include "game/Behavior.h"
#include "game/Body.h"
#include "game/ChatUI.h"
#include "game/Events.h"
#include "game/MainThread.h"
#include "game/Methods.h"
#include "game/Papyrus.h"
#include "game/Vision.h"
#include "game/Voice.h"
#include "net/Server.h"

namespace
{
	std::string g_token;

	void InitLogger()
	{
		auto path = logger::log_directory();
		if (!path) {
			return;
		}
		*path /= ANIMUNCULI_NAME ".log";
		auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
		auto log = std::make_shared<spdlog::logger>("global", std::move(sink));
		log->set_level(spdlog::level::info);
		log->flush_on(spdlog::level::info);
		spdlog::set_default_logger(std::move(log));
		spdlog::set_pattern("[%H:%M:%S.%e] [%t] [%^%l%$] %v");
	}

	std::string RandomToken()
	{
		std::random_device rd;  // RtlGenRandom on MSVC
		std::string        out;
		for (int i = 0; i < 4; ++i) {
			out += std::format("{:08x}", rd());
		}
		return out;
	}

	void WriteDiscovery()
	{
		auto path = logger::log_directory();
		if (!path) {
			return;
		}
		*path /= ANIMUNCULI_NAME ".discovery.json";
		const auto& cfg = Settings::Get();
		const json  j{ { "host", cfg.bindAddress == "0.0.0.0" ? "127.0.0.1" : cfg.bindAddress },
			 { "port", Net::ActualPort() },
			 { "token", g_token },
			 { "pid", GetCurrentProcessId() },
			 { "plugin_version", ANIMUNCULI_VERSION_STRING },
			 { "game_version", REL::Module::get().version().string() },
			 { "protocol", 1 } };
		std::ofstream out{ *path, std::ios::trunc };
		out << j.dump(2);
		logger::info("discovery file written: {}", path->string());
	}

	void StartServer()
	{
		const auto& cfg = Settings::Get();
		if (!cfg.serverEnabled) {
			logger::info("server disabled in INI");
			return;
		}
		g_token = cfg.token.empty() ? RandomToken() : cfg.token;
		if (cfg.bindAddress != "127.0.0.1" && cfg.bindAddress != "localhost") {
			logger::warn("server binds to {} - reachable from other machines; keep the token secret", cfg.bindAddress);
		}
		Net::ServerConfig sc;
		sc.bindAddress = cfg.bindAddress == "localhost" ? "127.0.0.1" : cfg.bindAddress;
		sc.port = cfg.port;
		sc.token = g_token;
		const bool ok = Net::Start(sc, Game::Methods::Hello, [](int a_level, const std::string& a_msg) {
			switch (a_level) {
			case 0: logger::info("net: {}", a_msg); break;
			case 1: logger::warn("net: {}", a_msg); break;
			default: logger::error("net: {}", a_msg); break;
			}
		});
		if (ok) {
			WriteDiscovery();
		}
	}

	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			Game::Events::Install();
			Game::MainThread::InstallFrameHook();
			Game::Voice::StartAudioThread();
			Game::ChatUI::Install();
			if (!Game::Vision::Install()) {
				logger::info("vision: renderer not ready at data load; will retry");
			}
			StartServer();
			break;
		case SKSE::MessagingInterface::kPreLoadGame:
			for (const auto& [agent, id] : Game::Agents::Snapshot()) {
				Game::Voice::Stop(agent);
			}
			break;
		case SKSE::MessagingInterface::kPostLoadGame:
			Game::Vision::Install();
			Game::Behavior::OnGameLoaded();
			Game::Actions::OnGameLoaded();
			Game::Events::Log().Push("game_loaded", { { "ok", a_msg->data != nullptr } });
			break;
		case SKSE::MessagingInterface::kNewGame:
			Game::Vision::Install();
			Game::Actions::OnGameLoaded();
			Game::Events::Log().Push("game_loaded", { { "new_game", true } });
			break;
		default:
			break;
		}
	}
}

SKSEPluginInfo(
	.Version = REL::Version{ ANIMUNCULI_VERSION_MAJOR, ANIMUNCULI_VERSION_MINOR, ANIMUNCULI_VERSION_PATCH, 0 },
	.Name = ANIMUNCULI_NAME ""sv,
	.Author = "tel-0s"sv,
	.RuntimeCompatibility = SKSE::PluginDeclaration::RuntimeCompatibility{ SKSE::VersionIndependence::AddressLibrary })

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	InitLogger();
	SKSE::Init(a_skse, false);
	logger::info("{} v{} loading (game {})", ANIMUNCULI_NAME, ANIMUNCULI_VERSION_STRING, REL::Module::get().version().string());

	if (REL::Module::IsVR()) {
		logger::error("Skyrim VR is not supported; staying inactive");
		return true;
	}

	Settings::Load();
	if (Settings::Get().logLevel == "debug") {
		spdlog::set_level(spdlog::level::debug);
	}

	Game::Methods::RegisterAll();
	Game::Agents::InstallSerialization();
	Game::MainThread::AddFrameCallback(Game::Events::Tick);
	Game::MainThread::AddFrameCallback(Game::Voice::Tick);
	Game::MainThread::AddFrameCallback(Game::ChatUI::Tick);
	Game::MainThread::AddFrameCallback(Game::Vision::Tick);
	Game::MainThread::AddFrameCallback(Game::Behavior::Tick);
	Game::MainThread::AddFrameCallback(Game::Body::Tick);
	Game::MainThread::AddFrameCallback(Game::Actions::Tick);

	SKSE::GetPapyrusInterface()->Register(Game::Papyrus::RegisterNatives);
	SKSE::GetMessagingInterface()->RegisterListener(OnMessage);
	return true;
}
