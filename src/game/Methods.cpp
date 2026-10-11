#include "PCH.h"
#include "Methods.h"
#include "Actions.h"
#include "Agents.h"
#include "Behavior.h"
#include "Body.h"
#include "ChatUI.h"
#include "Describe.h"
#include "Events.h"
#include "Guard.h"
#include "Knowledge.h"
#include "MainThread.h"
#include "Papyrus.h"
#include "Targets.h"
#include "Vision.h"
#include "Voice.h"

#include "Settings.h"
#include "core/Text.h"
#include "core/Wav.h"
#include "net/Rpc.h"
#include "net/Server.h"

namespace Game::Methods
{
	namespace
	{
		using Net::Affinity;
		using Net::RpcError;
		constexpr float kU = Targets::kUnitsPerMeter;

		float RadiusM(const json& p)
		{
			const auto& cfg = Settings::Get();
			return std::clamp(static_cast<float>(Net::OptNumber(p, "radius_m", cfg.defaultRadiusM)), 1.f, cfg.maxRadiusM);
		}

		std::vector<std::string> StringList(const json& p, const char* a_key)
		{
			std::vector<std::string> out;
			if (auto it = p.find(a_key); it != p.end() && it->is_array()) {
				for (auto& v : *it) {
					if (v.is_string()) {
						out.push_back(v.get<std::string>());
					}
				}
			}
			return out;
		}

		// The acting/perceiving body, or the player when no body is bound (perception only).
		RE::Actor* ViewerOrPlayer(const json& p, bool& a_bound)
		{
			auto* body = Agents::Get(Agents::AgentOf(p));
			a_bound = body && body->Is3DLoaded() && body->GetParentCell();
			return a_bound ? body : RE::PlayerCharacter::GetSingleton();
		}

		json Status()
		{
			json j;
			auto* ui = RE::UI::GetSingleton();
			j["in_game"] = MainThread::InGame();
			j["paused"] = ui && ui->GameIsPaused();
			json menus = json::array();
			for (auto name : { RE::DialogueMenu::MENU_NAME, RE::InventoryMenu::MENU_NAME, RE::ContainerMenu::MENU_NAME,
					 RE::BarterMenu::MENU_NAME, RE::MapMenu::MENU_NAME, RE::JournalMenu::MENU_NAME, RE::SleepWaitMenu::MENU_NAME,
					 RE::LoadingMenu::MENU_NAME, RE::MainMenu::MENU_NAME, RE::Console::MENU_NAME }) {
				if (ui && ui->IsMenuOpen(name)) {
					menus.push_back(std::string(name));
				}
			}
			j["menus"] = menus;
			j["agents"] = Agents::Describe();
			if (MainThread::InGame()) {
				auto* player = RE::PlayerCharacter::GetSingleton();
				j["player"] = { { "name", player->GetDisplayFullName() }, { "level", player->GetLevel() } };
				j["game_time"] = Describe::Time();
			}
			const auto& cfg = Settings::Get();
			j["features"] = { { "voice", cfg.voiceEnabled }, { "vision", cfg.visionEnabled && Vision::Installed() },
				{ "agent_perspective", cfg.allowAgentPerspective }, { "actions", cfg.actionsEnabled },
				{ "console", cfg.allowConsole }, { "chat_window", ChatUI::Available() } };
			return j;
		}

		json Observe(const json& p)
		{
			MainThread::RequireInGame();
			bool       bound = false;
			auto*      viewer = ViewerOrPlayer(p, bound);
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const auto agent = Agents::AgentOf(p);
			const auto radius = RadiusM(p);

			json j;
			j["time"] = Describe::Time();
			j["location"] = Describe::Location(viewer);
			if (auto landmark = Knowledge::NearestPlace(viewer); !landmark.is_null()) {
				j["location"]["near"] = std::move(landmark);
			}
			if (bound) {
				j["self"] = Describe::ActorState(viewer, viewer, true);
				j["player"] = Describe::ActorState(player, viewer, false);
				j["speaking"] = Voice::IsSpeaking(agent);
			} else {
				j["self"] = nullptr;
				j["note"] = Agents::Get(agent) ? "your body is not in the loaded area; perceiving from the player's position" :
				                                 "no body bound; perceiving from the player's position (see bind)";
				j["player"] = Describe::ActorState(player, nullptr, true);
			}

			Describe::NearbyOptions actors;
			actors.radiusM = radius;
			actors.kinds = { "actor" };
			actors.limit = static_cast<std::size_t>(std::clamp(Net::OptNumber(p, "max_actors", 12), 0.0, 60.0)) + 1;
			actors.losOnly = Settings::Get().requireLineOfSight && bound;
			json a = Describe::Nearby(viewer, actors);
			json filtered = json::array();
			for (auto& e : a) {
				if (!e.value("is_player", false) && filtered.size() + 1 < actors.limit) {
					filtered.push_back(std::move(e));
				}
			}
			j["actors"] = std::move(filtered);

			Describe::NearbyOptions objs;
			objs.radiusM = std::min(radius, 25.f);
			objs.kinds = { "item", "container", "door", "furniture", "activator", "flora" };
			objs.limit = static_cast<std::size_t>(std::clamp(Net::OptNumber(p, "max_objects", 12), 0.0, 60.0));
			objs.losOnly = actors.losOnly;
			j["objects"] = Describe::Nearby(viewer, objs);
			return j;
		}

		json Nearby(const json& p)
		{
			MainThread::RequireInGame();
			bool                    bound = false;
			auto*                   viewer = ViewerOrPlayer(p, bound);
			Describe::NearbyOptions o;
			o.radiusM = RadiusM(p);
			o.kinds = StringList(p, "kinds");
			o.limit = static_cast<std::size_t>(std::clamp(Net::OptNumber(p, "limit", 25), 1.0, 200.0));
			o.losOnly = Net::OptBool(p, "los_only", false) || (Settings::Get().requireLineOfSight && bound);
			return { { "items", Describe::Nearby(viewer, o) } };
		}

		json Inspect(const json& p)
		{
			MainThread::RequireInGame();
			bool  bound = false;
			auto* viewer = ViewerOrPlayer(p, bound);
			auto* target = Targets::Ref(Net::RequireString(p, "target"), bound ? viewer : nullptr, Settings::Get().maxRadiusM * kU);
			return Describe::Detailed(target, viewer);
		}

		json Inventory(const json& p)
		{
			MainThread::RequireInGame();
			const auto who = Core::Lower(Net::OptString(p, "who", "self"));
			auto*      body = Agents::Get(Agents::AgentOf(p));
			RE::TESObjectREFR* owner = nullptr;
			if (who == "self") {
				owner = Agents::Require(p, false);
			} else if (who == "player") {
				if (!Settings::Get().allowReadPlayerInventory) {
					throw RpcError("disabled", "reading the player's inventory is disabled ([Actions] AllowReadPlayerInventory)");
				}
				owner = RE::PlayerCharacter::GetSingleton();
			} else {
				owner = Targets::Ref(who, body);
				if (!body) {
					throw RpcError("not_bound", "you need a body to look into containers");
				}
				if (owner->GetPosition().GetDistance(body->GetPosition()) / kU > 6.f) {
					throw RpcError("failed", "too far away to see what is inside; get closer");
				}
			}
			const auto limit = static_cast<std::size_t>(std::clamp(Net::OptNumber(p, "limit", 60), 1.0, 500.0));
			return Describe::Inventory(owner, Net::OptString(p, "filter"), limit);
		}

		json Bind(const json& p)
		{
			MainThread::RequireInGame();
			const auto agent = Agents::AgentOf(p);
			auto*      actor = Targets::Actor(Net::OptString(p, "target", "crosshair"), nullptr);
			if (actor->IsPlayerRef()) {
				throw RpcError("bad_request", "an agent cannot inhabit the player");
			}
			if (actor->IsDead()) {
				throw RpcError("failed", "cannot inhabit the dead");
			}
			Agents::Bind(agent, actor);
			return { { "agent", agent }, { "id", Core::FormatFormID(actor->GetFormID()) }, { "name", actor->GetDisplayFullName() },
				{ "follower", actor->IsPlayerTeammate() } };
		}

		json Say(const json& p)
		{
			const auto               text = Net::OptString(p, "text");
			std::optional<Core::PcmClip> clip;
			std::vector<std::uint8_t> bytes;
			if (const auto b64 = Net::OptString(p, "audio_b64"); !b64.empty()) {
				auto decoded = Core::Base64Decode(b64);
				if (!decoded) {
					throw RpcError("bad_request", "audio_b64 is not valid base64");
				}
				bytes = std::move(*decoded);
			} else if (const auto path = Net::OptString(p, "audio_path"); !path.empty()) {
				const std::filesystem::path fp{ std::u8string(path.begin(), path.end()) };
				if (Core::Lower(fp.extension().string()) != ".wav") {
					throw RpcError("bad_request", "audio_path must be a .wav file");
				}
				std::ifstream in{ fp, std::ios::binary };
				if (!in) {
					throw RpcError("not_found", "cannot read " + path);
				}
				bytes.assign(std::istreambuf_iterator<char>(in), {});
			}
			if (!bytes.empty()) {
				Core::PcmClip c;
				std::string   err;
				if (!Core::DecodeWav(bytes, c, err)) {
					throw RpcError("bad_request", err);
				}
				if (c.seconds() > 120.0) {
					throw RpcError("bad_request", "voice clips are limited to 2 minutes");
				}
				clip = std::move(c);
			}
			if (text.empty() && !clip) {
				throw RpcError("bad_request", "say needs text and/or audio");
			}
			return MainThread::Run([&]() -> json {
				MainThread::RequireInGame();
				Voice::SayRequest req;
				req.agent = Agents::AgentOf(p);
				req.actor = Agents::Require(p, true);
				req.text = text;
				req.clip = std::move(clip);
				req.emotion = Net::OptString(p, "emotion");
				req.subtitle = Net::OptBool(p, "subtitle", true);
				req.interrupt = Net::OptBool(p, "interrupt", false);
				if (const auto t = Net::OptString(p, "target"); !t.empty()) {
					req.lookAt = Targets::Ref(t, req.actor);
				}
				return Voice::Say(std::move(req));
			}, 5s);
		}

		json Events(const json& p, bool a_wait)
		{
			const auto since = static_cast<std::uint64_t>(std::max(0.0, Net::OptNumber(p, "since", 0)));
			const auto limit = static_cast<std::size_t>(std::clamp(Net::OptNumber(p, "limit", 100), 1.0, 500.0));
			const auto types = StringList(p, "types");
			Core::EventLog::Result r;
			if (a_wait) {
				const auto ms = static_cast<std::int64_t>(std::clamp(Net::OptNumber(p, "timeout_ms", 30000), 0.0, 120000.0));
				r = Game::Events::Log().Wait(since, types, limit, std::chrono::milliseconds(ms));
			} else {
				r = Game::Events::Log().Query(since, types, limit);
			}
			return { { "events", std::move(r.events) }, { "next_seq", r.nextSeq }, { "dropped", r.dropped } };
		}

		void Main(const char* a_name, json (*a_fn)(const json&))
		{
			Net::Register(a_name, Affinity::kMain, a_fn);
		}

		// Read-only perception: a structured exception (an access violation on an unusual object)
		// becomes an error for the agent instead of a crash for the player. See Guard.h.
		void Perceive(const char* a_name, json (*a_fn)(const json&))
		{
			Net::Register(a_name, Affinity::kMain, [a_name, a_fn](const json& p) -> json {
				struct Ctx
				{
					json (*fn)(const json&);
					const json*        params;
					json               out;
					std::exception_ptr error;
				} ctx{ a_fn, &p, {}, nullptr };
				auto thunk = [](void* a_ctx) {
					auto* c = static_cast<Ctx*>(a_ctx);
					try {
						c->out = c->fn(*c->params);
					} catch (...) {
						c->error = std::current_exception();
					}
				};
				Guard::Fault fault;
				if (!Guard::Run(thunk, &ctx, fault)) {
					const auto where = Guard::Describe(fault.address);
					logger::error("{}: caught exception {:#x} at {} (request: {}); the game kept running", a_name, fault.code, where,
						p.dump(-1, ' ', false, json::error_handler_t::replace).substr(0, 300));
					throw RpcError("internal", std::format("the game faulted while reading that ({:#x} at {}); it was contained and logged. Avoid repeating the same request", fault.code, where));
				}
				if (ctx.error) {
					std::rethrow_exception(ctx.error);
				}
				return std::move(ctx.out);
			});
		}
	}

	json Hello()
	{
		const auto ver = REL::Module::get().version();
		return { { "plugin", ANIMUNCULI_NAME }, { "plugin_version", ANIMUNCULI_VERSION_STRING },
			{ "game_version", ver.string() }, { "runtime", REL::Module::IsAE() ? "AE" : "SE" },
			{ "event_seq", Game::Events::Log().NextSeq() } };
	}

	void RegisterAll()
	{
		Net::SetMainRunner(MainThread::Run);

		Net::Register("ping", Affinity::kWorker, [](const json&) { return json{ { "pong", true } }; });
		Net::Register("status", Affinity::kWorker, [](const json&) {
			try {
				return MainThread::Run(Status, 1500ms);
			} catch (const RpcError&) {
				return json{ { "in_game", false }, { "loading", true }, { "agents", json::array() } };
			}
		});
		Net::Register("agents", Affinity::kMain, [](const json&) { return json{ { "agents", Agents::Describe() } }; });
		Main("bind", Bind);
		Net::Register("unbind", Affinity::kWorker, [](const json& p) { return json{ { "ok", Agents::Unbind(Agents::AgentOf(p)) } }; });

		Perceive("observe", Observe);
		Perceive("nearby", Nearby);
		Perceive("inspect", Inspect);
		Perceive("inventory", Inventory);
		Perceive("quests", [](const json&) -> json {
			MainThread::RequireInGame();
			return Describe::Quests();
		});
		Perceive("crosshair", [](const json& p) -> json {
			MainThread::RequireInGame();
			auto* pick = RE::CrosshairPickData::GetSingleton();
			auto  ref = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
			if (!ref) {
				return { { "target", nullptr } };
			}
			bool bound = false;
			return { { "target", Describe::Brief(ref.get(), ViewerOrPlayer(p, bound)) } };
		});

		Net::Register("events", Affinity::kWorker, [](const json& p) { return Events(p, false); });
		Net::Register("wait_events", Affinity::kWorker, [](const json& p) { return Events(p, true); });

		Net::Register("say", Affinity::kWorker, Say);
		Main("stop_speaking", [](const json& p) {
			Voice::Stop(Agents::AgentOf(p));
			return json{ { "ok", true } };
		});

		Net::Register("capture", Affinity::kWorker, Vision::Capture);

		Perceive("read", Knowledge::Read);
		Perceive("map", Knowledge::Map);
		Perceive("locate", Knowledge::Locate);
		Perceive("dialogue_options", Knowledge::DialogueOptions);

		Main("sit", Body::Sit);
		Main("stand", Body::Stand);
		Perceive("spells", Body::Spells);
		Main("learn_spell", Body::LearnSpell);
		Net::Register("gestures", Affinity::kWorker, Body::Gestures);

		Main("set_combat", Behavior::SetCombat);
		Main("set_follow_style", Behavior::SetFollowStyle);
		Net::Register("orders", Affinity::kWorker, [](const json& p) { return Behavior::Describe(Agents::AgentOf(p)); });

		// The agent's host reports when it is thinking (MCPL inference/lifecycle): shown in the chat
		// window, and sent to Papyrus so a follower mod can play a thinking idle.
		Main("presence", [](const json& p) -> json {
			const bool thinking = Net::OptBool(p, "thinking", false);
			ChatUI::SetThinking(thinking);
			if (auto* body = Agents::Get(Agents::AgentOf(p))) {
				Papyrus::SendModEvent("Animunculi_Thinking", Agents::AgentOf(p), thinking ? 1.f : 0.f, body);
			}
			return { { "ok", true } };
		});

		Main("move_to", Actions::MoveTo);
		Main("follow", Actions::Follow);
		Main("wait_here", Actions::WaitHere);
		Main("activate", Actions::Activate);
		Main("pick_up", Actions::PickUp);
		Main("mount", Actions::Mount);
		Main("dismount", Actions::Dismount);
		Main("give", Actions::Give);
		Main("use_item", Actions::UseItem);
		Main("unequip", Actions::Unequip);
		Main("cast", Actions::Cast);
		Main("attack", Actions::Attack);
		Main("stop_combat", Actions::StopCombat);
		Main("look_at", Actions::LookAt);
		Main("emote", Actions::Emote);
		Main("draw_weapon", Actions::DrawWeapon);
		Main("open_inventory", Actions::OpenInventory);
		Main("notify", Actions::Notify);
		Main("console", Actions::Console);

		Net::Register("actions", Affinity::kWorker, [](const json&) {
			json list = json::array();
			for (auto& a : Papyrus::Actions()) {
				list.push_back({ { "name", a.name }, { "description", a.description }, { "params", a.params } });
			}
			return json{ { "actions", list } };
		});
		Main("perform", [](const json& p) -> json {
			MainThread::RequireInGame();
			auto* actor = Agents::Require(p, true);
			const auto name = Net::RequireString(p, "name");
			const json args = p.contains("args") && p["args"].is_object() ? p["args"] : json::object();
			return { { "action_id", Papyrus::Perform(Agents::AgentOf(p), actor, name, args) } };
		});
	}
}
