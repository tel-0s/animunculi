#include "PCH.h"
#include "ChatUI.h"
#include "Agents.h"
#include "Events.h"
#include "MainThread.h"

#include "Settings.h"
#include "net/Server.h"

#pragma warning(push, 0)
#include "SKSEMenuFramework.h"
#pragma warning(pop)

namespace Game::ChatUI
{
	namespace
	{
		namespace G = ImGuiMCP;

		struct Entry
		{
			std::string who;
			std::string text;
			int         kind;  // 0 player, 1 agent, 2 overheard, 3 system
		};

		std::mutex         g_lock;
		std::deque<Entry>  g_history;
		std::uint64_t      g_eventCursor{ 0 };
		std::string        g_status{ "starting..." };
		bool               g_scrollToBottom{ false };
		bool               g_focusInput{ false };
		char               g_input[1024]{};
		bool               g_available{ false };
		std::atomic<bool>  g_thinking{ false };
		SKSEMenuFramework::Model::WindowInterface* g_window{ nullptr };

		void Add(Entry a_entry)
		{
			std::lock_guard lock{ g_lock };
			g_history.push_back(std::move(a_entry));
			while (g_history.size() > 200) {
				g_history.pop_front();
			}
			g_scrollToBottom = true;
		}

		void PullEvents()
		{
			static const std::vector<std::string> kTypes{ "player_chat", "speech_started", "heard", "player_dialogue", "agent_bound" };
			auto r = Events::Log().Query(g_eventCursor, kTypes, 50);
			g_eventCursor = r.nextSeq;
			for (auto& ev : r.events) {
				const auto type = ev.value("type", "");
				if (type == "player_chat") {
					Add({ "You", ev.value("text", ""), 0 });
				} else if (type == "speech_started") {
					Add({ ev.value("speaker", ev.value("agent", "Agent")), ev.value("text", ""), 1 });
				} else if (type == "heard") {
					const auto who = ev.contains("speaker") ? ev["speaker"].value("name", "Someone") : std::string("Someone");
					Add({ who, ev.value("text", ""), 2 });
				} else if (type == "player_dialogue") {
					Add({ "You (dialogue)", ev.value("text", ""), 0 });
				} else if (type == "agent_bound") {
					Add({ "", "Agent '" + ev.value("agent", "") + "' now inhabits " + ev.value("name", "someone") + ".", 3 });
				}
			}
		}

		void Submit()
		{
			std::string text{ g_input };
			while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
				text.pop_back();
			}
			g_input[0] = '\0';
			if (!text.empty()) {
				Events::PlayerChat(text, "chat_window");
			}
			g_focusInput = true;
		}

		void __stdcall RenderChat()
		{
			PullEvents();
			G::SetNextWindowSize(G::ImVec2{ 560.f, 380.f }, G::ImGuiCond_FirstUseEver);
			if (G::Begin("Animunculi##chat", nullptr, G::ImGuiWindowFlags_NoCollapse)) {
				std::string status;
				{
					std::lock_guard lock{ g_lock };
					status = g_status;
				}
				G::TextColored(G::ImVec4{ 0.65f, 0.65f, 0.65f, 1.f }, "%s", status.c_str());
				G::Separator();
				G::BeginChild("history", G::ImVec2{ 0.f, -34.f }, G::ImGuiChildFlags_Border);
				{
					std::lock_guard lock{ g_lock };
					for (const auto& e : g_history) {
						static const G::ImVec4 kColors[] = {
							{ 0.55f, 0.80f, 1.00f, 1.f },  // player
							{ 1.00f, 0.85f, 0.55f, 1.f },  // agent
							{ 0.70f, 0.70f, 0.70f, 1.f },  // overheard
							{ 0.60f, 0.90f, 0.60f, 1.f },  // system
						};
						if (!e.who.empty()) {
							G::TextColored(kColors[e.kind], "%s:", e.who.c_str());
							G::SameLine();
						}
						G::PushTextWrapPos(0.f);
						G::TextUnformatted(e.text.c_str());
						G::PopTextWrapPos();
					}
					if (g_scrollToBottom) {
						G::SetScrollHereY(1.f);
						g_scrollToBottom = false;
					}
				}
				G::EndChild();
				if (g_focusInput || G::IsWindowAppearing()) {
					G::SetKeyboardFocusHere();
					g_focusInput = false;
				}
				G::PushItemWidth(-70.f);
				if (G::InputText("##say", g_input, sizeof(g_input), G::ImGuiInputTextFlags_EnterReturnsTrue)) {
					Submit();
				}
				G::PopItemWidth();
				G::SameLine();
				if (G::Button("Send")) {
					Submit();
				}
			}
			G::End();
			if (G::IsKeyPressed(G::ImGuiKey_Escape) && g_window) {
				g_window->IsOpen = false;
			}
		}

		void __stdcall RenderStatus()
		{
			std::string status;
			{
				std::lock_guard lock{ g_lock };
				status = g_status;
			}
			G::TextUnformatted(status.c_str());
			G::Separator();
			G::TextWrapped("Bind the default agent to a follower: point the crosshair at them, then press the button (or use bind_body from the agent).");
			if (G::Button("Bind agent to crosshair target")) {
				SKSE::GetTaskInterface()->AddTask([]() {
					auto* pick = RE::CrosshairPickData::GetSingleton();
					auto  ref = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
					if (auto* actor = ref ? ref->As<RE::Actor>() : nullptr) {
						Agents::Bind(std::string(Agents::kDefault), actor);
						RE::SendHUDMessage::ShowHUDMessage((std::string("Animunculi: agent now inhabits ") + actor->GetDisplayFullName()).c_str(), nullptr, false);
					} else {
						RE::SendHUDMessage::ShowHUDMessage("Animunculi: point at an NPC first", nullptr, false);
					}
				});
			}
			G::SameLine();
			if (G::Button("Unbind")) {
				Agents::Unbind(std::string(Agents::kDefault));
			}
			if (G::Button("Open chat window")) {
				Open();
			}
		}

		class HotkeySink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!a_event || !g_window) {
					return RE::BSEventNotifyControl::kContinue;
				}
				for (auto* e = *a_event; e; e = e->next) {
					auto* b = e->AsButtonEvent();
					if (!b || b->GetDevice() != RE::INPUT_DEVICE::kKeyboard || !b->IsDown()) {
						continue;
					}
					if (b->GetIDCode() != Settings::Get().chatHotkey) {
						continue;
					}
					auto* ui = RE::UI::GetSingleton();
					if (!MainThread::InGame() || (ui && (ui->IsMenuOpen(RE::Console::MENU_NAME) || ui->GameIsPaused()))) {
						continue;
					}
					g_window->IsOpen = !g_window->IsOpen;
					if (g_window->IsOpen) {
						g_focusInput = true;
					}
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};
	}

	void Install()
	{
		if (!Settings::Get().chatEnabled) {
			return;
		}
		if (!GetModuleHandleW(L"SKSEMenuFramework")) {
			logger::info("chat: SKSE Menu Framework not installed; the chat window is unavailable (Papyrus Animunculi.PlayerSay still works)");
			return;
		}
		g_eventCursor = Events::Log().NextSeq();
		SKSEMenuFramework::SetSection("Animunculi");
		SKSEMenuFramework::AddSectionItem("Status", RenderStatus);
		g_window = SKSEMenuFramework::AddWindow(RenderChat, Settings::Get().chatPausesGame);
		if (!g_window) {
			logger::warn("chat: SKSE Menu Framework refused the window");
			return;
		}
		static HotkeySink sink;
		if (auto* input = RE::BSInputDeviceManager::GetSingleton()) {
			input->AddEventSink(&sink);
		}
		g_available = true;
		logger::info("chat: window registered (hotkey scan code {:#x})", Settings::Get().chatHotkey);
	}

	void Open()
	{
		if (g_window) {
			g_window->IsOpen = true;
			g_focusInput = true;
		}
	}

	bool Available() { return g_available; }

	void Tick(float)
	{
		static std::uint32_t frame = 0;
		if (++frame % 30 != 0) {
			return;
		}
		std::string s;
		const int   clients = Net::ClientCount();
		s = clients > 0 ? std::format("Connected: {}", Net::ClientSummary()) : std::string("No agent connected");
		auto* body = Agents::Get(Agents::kDefault);
		s += body ? std::format("  |  Body: {}", body->GetDisplayFullName()) : std::string("  |  No body bound");
		if (g_thinking && body) {
			s += std::format("  |  {} is thinking...", body->GetDisplayFullName());
		}
		std::lock_guard lock{ g_lock };
		g_status = std::move(s);
	}

	void SetThinking(bool a_thinking)
	{
		g_thinking = a_thinking;
	}
}
