#include "PCH.h"
#include "Events.h"
#include "Agents.h"
#include "Describe.h"
#include "Targets.h"

#include "core/ItemBuffer.h"
#include "core/Text.h"

namespace Game::Events
{
	namespace
	{
		constexpr float kU = Targets::kUnitsPerMeter;

		using Clock = std::chrono::steady_clock;

		// Suppresses repeats of the same key within a window (hits, combat flapping, subtitles).
		class Throttle
		{
		public:
			bool Allow(const std::string& a_key, std::chrono::milliseconds a_window)
			{
				std::lock_guard lock{ _m };
				const auto      now = Clock::now();
				if (_seen.size() > 512) {
					std::erase_if(_seen, [&](const auto& kv) { return now - kv.second > 60s; });
				}
				auto [it, inserted] = _seen.try_emplace(a_key, now);
				if (!inserted) {
					if (now - it->second < a_window) {
						return false;
					}
					it->second = now;
				}
				return true;
			}

		private:
			std::mutex                                         _m;
			std::unordered_map<std::string, Clock::time_point> _seen;
		};

		Throttle g_throttle;

		Core::ItemBuffer g_items;  // cancels scripted add/remove blips (see core/ItemBuffer.h)

		void FlushItems()
		{
			for (auto& c : g_items.Ready(Clock::now())) {
				Log().Push(c.received ? "item_received" : "item_removed", std::move(c.payload));
			}
		}

		std::string Id(const RE::TESForm* f) { return f ? Core::FormatFormID(f->GetFormID()) : ""; }

		std::string NameOf(RE::TESObjectREFR* r) { return Describe::Name(r); }

		bool IsPlayer(const RE::TESObjectREFR* r) { return r && r->IsPlayerRef(); }

		bool Involved(RE::TESObjectREFR* r) { return r && (IsPlayer(r) || Agents::IsAgent(r)); }

		float DistToPlayerM(RE::TESObjectREFR* r)
		{
			auto* p = RE::PlayerCharacter::GetSingleton();
			if (!r || !p) {
				return 1e9f;
			}
			return r->GetPosition().GetDistance(p->GetPosition()) / kU;
		}

		json Who(RE::TESObjectREFR* r)
		{
			if (!r) {
				return nullptr;
			}
			json j{ { "id", Id(r) }, { "name", NameOf(r) } };
			if (IsPlayer(r)) {
				j["is_player"] = true;
			} else if (const auto agent = Agents::AgentFor(r->GetFormID()); !agent.empty()) {
				j["agent"] = agent;
			}
			return j;
		}

		class Sinks final :
			public RE::BSTEventSink<RE::TESCombatEvent>,
			public RE::BSTEventSink<RE::TESDeathEvent>,
			public RE::BSTEventSink<RE::TESHitEvent>,
			public RE::BSTEventSink<RE::TESContainerChangedEvent>,
			public RE::BSTEventSink<RE::TESActorLocationChangeEvent>,
			public RE::BSTEventSink<RE::TESQuestStageEvent>,
			public RE::BSTEventSink<RE::TESActivateEvent>,
			public RE::BSTEventSink<RE::TESSpellCastEvent>,
			public RE::BSTEventSink<RE::TESEnterBleedoutEvent>,
			public RE::BSTEventSink<RE::MenuOpenCloseEvent>,
			public RE::BSTEventSink<RE::LevelIncrease::Event>
		{
			using C = RE::BSEventNotifyControl;

		public:
			static Sinks* Get()
			{
				static Sinks s;
				return &s;
			}

			C ProcessEvent(const RE::TESCombatEvent* e, RE::BSTEventSource<RE::TESCombatEvent>*) override
			{
				if (!e || !e->actor) {
					return C::kContinue;
				}
				auto* actor = e->actor.get();
				auto* target = e->targetActor.get();
				if (!Involved(actor) && !Involved(target)) {
					return C::kContinue;
				}
				const auto state = e->newState.get();
				const char* s = state == RE::ACTOR_COMBAT_STATE::kCombat ? "combat" : state == RE::ACTOR_COMBAT_STATE::kSearching ? "searching" : "none";
				if (!g_throttle.Allow("combat" + Id(actor) + Id(target) + s, 2s)) {
					return C::kContinue;
				}
				json j{ { "actor", Who(actor) }, { "state", s } };
				if (target) {
					j["target"] = Who(target);
				}
				Log().Push("combat", std::move(j));
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESDeathEvent* e, RE::BSTEventSource<RE::TESDeathEvent>*) override
			{
				if (!e || !e->actorDying) {
					return C::kContinue;
				}
				auto* victim = e->actorDying.get();
				auto* killer = e->actorKiller.get();
				if (!Involved(victim) && !Involved(killer) && DistToPlayerM(victim) > 60.f) {
					return C::kContinue;
				}
				if (!g_throttle.Allow("death" + Id(victim), 10s)) {  // the event fires when dying and when dead
					return C::kContinue;
				}
				json j{ { "victim", Who(victim) } };
				if (killer) {
					j["killer"] = Who(killer);
				}
				Log().Push("death", std::move(j));
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESHitEvent* e, RE::BSTEventSource<RE::TESHitEvent>*) override
			{
				if (!e || !e->target) {
					return C::kContinue;
				}
				auto* target = e->target.get();
				auto* cause = e->cause.get();
				if (!Involved(target) && !Involved(cause)) {
					return C::kContinue;
				}
				if (!g_throttle.Allow("hit" + Id(target) + Id(cause), 1500ms)) {
					return C::kContinue;
				}
				json j{ { "target", Who(target) } };
				if (cause) {
					j["attacker"] = Who(cause);
				}
				if (auto* src = RE::TESForm::LookupByID(e->source)) {
					if (const char* n = src->GetName(); n && *n) {
						j["with"] = n;
					}
				}
				using F = RE::TESHitEvent::Flag;
				if (e->flags.any(F::kHitBlocked)) {
					j["blocked"] = true;
				}
				if (e->flags.any(F::kPowerAttack)) {
					j["power_attack"] = true;
				}
				if (e->flags.any(F::kSneakAttack)) {
					j["sneak_attack"] = true;
				}
				if (auto* ta = target->As<RE::Actor>()) {
					const float max = ta->GetActorValueMax(RE::ActorValue::kHealth);
					if (max > 0.f) {
						j["target_health"] = std::round(std::clamp(ta->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) / max, 0.f, 1.f) * 100.f) / 100.f;
					}
				}
				Log().Push("hit", std::move(j));
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESContainerChangedEvent* e, RE::BSTEventSource<RE::TESContainerChangedEvent>*) override
			{
				if (!e || e->itemCount == 0) {
					return C::kContinue;
				}
				const auto toAgent = Agents::AgentFor(e->newContainer);
				const auto fromAgent = Agents::AgentFor(e->oldContainer);
				if (toAgent.empty() && fromAgent.empty()) {
					return C::kContinue;
				}
				auto* item = RE::TESForm::LookupByID(e->baseObj);
				const char* itemName = item ? item->GetName() : nullptr;
				if (!itemName || !*itemName) {
					return C::kContinue;
				}
				auto nameOfContainer = [](RE::FormID id) -> json {
					if (!id) {
						return nullptr;
					}
					auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
					return ref ? Who(ref) : json(nullptr);
				};
				// How the item moved. A world reference accompanies real pickups and drops; with neither
				// a container nor a reference, a script (or the game) created or deleted the item.
				auto*      player = RE::PlayerCharacter::GetSingleton();
				const bool hasRef = e->reference.native_handle() != 0;
				auto       isActor = [](RE::FormID id) {
                    auto* ref = RE::TESForm::LookupByID<RE::TESObjectREFR>(id);
                    return ref && ref->As<RE::Actor>();
				};
				const bool received = !toAgent.empty();
				json       j{ { "item", itemName }, { "item_id", Core::FormatFormID(e->baseObj) }, { "count", e->itemCount } };
				j["agent"] = received ? toAgent : fromAgent;
				if (received) {
					if (e->oldContainer) {
						j["from"] = nameOfContainer(e->oldContainer);
						j["how"] = player && e->oldContainer == player->GetFormID() ? "given_by_player" :
						           isActor(e->oldContainer)                          ? "given" :
						                                                               "taken_from_container";
					} else {
						j["how"] = hasRef ? "picked_up" : "spawned";
					}
				} else {
					if (e->newContainer) {
						j["to"] = nameOfContainer(e->newContainer);
						j["how"] = player && e->newContainer == player->GetFormID() ? "given_to_player" :
						           isActor(e->newContainer)                          ? "given" :
						                                                               "stored";
					} else {
						j["how"] = hasRef ? "dropped" : "removed";
					}
				}
				const auto agent = j["agent"].get<std::string>();
				if (!g_items.Add({ agent, e->baseObj, e->itemCount, received, std::move(j), Clock::now() })) {
					logger::debug("events: dropped transient item change ({:08X} x{} for '{}')", e->baseObj, e->itemCount, agent);
				}
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESActorLocationChangeEvent* e, RE::BSTEventSource<RE::TESActorLocationChangeEvent>*) override
			{
				if (!e || !IsPlayer(e->actor.get())) {
					return C::kContinue;
				}
				auto name = [](RE::BGSLocation* l) -> json {
					return l && l->GetFullName() && *l->GetFullName() ? json(l->GetFullName()) : json(nullptr);
				};
				json j{ { "from", name(e->oldLoc) }, { "to", name(e->newLoc) } };
				if (e->newLoc) {
					json kws = json::array();
					for (std::uint32_t i = 0; i < e->newLoc->numKeywords; ++i) {
						if (auto* kw = e->newLoc->keywords[i]) {
							std::string_view id = kw->GetFormEditorID() ? kw->GetFormEditorID() : "";
							if (id.starts_with("LocType")) {
								kws.push_back(std::string(id.substr(7)));
							}
						}
					}
					if (!kws.empty()) {
						j["types"] = kws;
					}
				}
				Log().Push("location_changed", std::move(j));
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESQuestStageEvent* e, RE::BSTEventSource<RE::TESQuestStageEvent>*) override
			{
				if (!e) {
					return C::kContinue;
				}
				auto* q = RE::TESForm::LookupByID<RE::TESQuest>(e->formID);
				if (!q) {
					return C::kContinue;
				}
				const char* name = q->GetFullName();
				if (!name || !*name || (q->GetType() == RE::QUEST_DATA::Type::kNone && !q->IsActive())) {
					return C::kContinue;  // background/radiant plumbing
				}
				Log().Push("quest_stage", { { "quest", name }, { "quest_id", Id(q) }, { "stage", e->stage } });
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESActivateEvent* e, RE::BSTEventSource<RE::TESActivateEvent>*) override
			{
				if (!e || !e->objectActivated || !Involved(e->actionRef.get())) {
					return C::kContinue;
				}
				auto*      target = e->objectActivated.get();
				const auto kind = Describe::Kind(target);
				const bool byAgent = Agents::IsAgent(e->actionRef.get());
				// The follower AI's own furniture use (sandboxing onto benches and chairs) is clockwork,
				// not something the agent did. `observe` still reports sitting.
				if (byAgent && kind == "furniture") {
					return C::kContinue;
				}
				if (NameOf(target).empty() || !g_throttle.Allow("act" + Id(e->actionRef.get()) + Id(target), byAgent ? 10s : 1s)) {
					return C::kContinue;
				}
				json j{ { "by", Who(e->actionRef.get()) }, { "target", Who(target) }, { "kind", kind } };
				Log().Push("activated", std::move(j));
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESSpellCastEvent* e, RE::BSTEventSource<RE::TESSpellCastEvent>*) override
			{
				if (!e || !Involved(e->object.get())) {
					return C::kContinue;
				}
				auto* spell = RE::TESForm::LookupByID(e->spell);
				const char* n = spell ? spell->GetName() : nullptr;
				if (!n || !*n || !g_throttle.Allow("cast" + Id(e->object.get()) + Id(spell), 1s)) {
					return C::kContinue;
				}
				Log().Push("spell_cast", { { "caster", Who(e->object.get()) }, { "spell", n } });
				return C::kContinue;
			}

			C ProcessEvent(const RE::TESEnterBleedoutEvent* e, RE::BSTEventSource<RE::TESEnterBleedoutEvent>*) override
			{
				if (e && Involved(e->actor.get())) {
					Log().Push("bleedout", { { "actor", Who(e->actor.get()) } });
				}
				return C::kContinue;
			}

			C ProcessEvent(const RE::MenuOpenCloseEvent* e, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
			{
				if (!e || e->menuName != RE::DialogueMenu::MENU_NAME) {
					return C::kContinue;
				}
				json j;
				if (auto* mtm = RE::MenuTopicManager::GetSingleton()) {
					auto speaker = (e->opening ? mtm->speaker : mtm->lastSpeaker).get();
					if (speaker) {
						j["with"] = Who(speaker.get());
					}
				}
				Log().Push(e->opening ? "dialogue_started" : "dialogue_ended", std::move(j));
				return C::kContinue;
			}

			C ProcessEvent(const RE::LevelIncrease::Event* e, RE::BSTEventSource<RE::LevelIncrease::Event>*) override
			{
				if (e) {
					Log().Push("level_up", { { "level", e->newLevel } });
				}
				return C::kContinue;
			}
		};

		// ---------------------------------------------------------------- polled sources

		RE::MenuTopicManager::Dialogue* g_lastDialogue{ nullptr };
		std::string                     g_lastDialogueText;
		std::unordered_map<RE::FormID, bool> g_agentLoaded;

		void EmitHeard(RE::TESObjectREFR* a_speaker, const std::string& a_text, bool a_inDialogue)
		{
			if (!a_speaker || a_text.empty() || Agents::IsAgent(a_speaker) || IsPlayer(a_speaker)) {
				return;
			}
			const auto key = "heard" + Id(a_speaker) + a_text;
			if (!g_throttle.Allow(key, 20s)) {
				return;
			}
			json j{ { "speaker", Who(a_speaker) }, { "text", a_text } };
			j["dist_m"] = std::round(DistToPlayerM(a_speaker) * 10.f) / 10.f;
			if (a_inDialogue) {
				j["to_player"] = true;
			}
			Log().Push("heard", std::move(j));
		}

		void PollSubtitles()
		{
			auto* sm = RE::SubtitleManager::GetSingleton();
			if (!sm) {
				return;
			}
			std::vector<std::pair<RE::ObjectRefHandle, std::string>> lines;
			{
				RE::BSSpinLockGuard guard{ sm->lock };
				for (auto& info : sm->subtitles) {
					const char* t = info.subtitle.c_str();
					if (t && *t) {
						lines.emplace_back(info.speaker, t);
					}
				}
			}
			for (auto& [handle, text] : lines) {
				if (auto speaker = handle.get()) {
					if (DistToPlayerM(speaker.get()) < 45.f) {
						EmitHeard(speaker.get(), text, false);
					}
				}
			}
		}

		void PollDialogue()
		{
			auto* mtm = RE::MenuTopicManager::GetSingleton();
			if (!mtm || !mtm->menuOpen) {
				g_lastDialogue = nullptr;
				g_lastDialogueText.clear();
				return;
			}
			auto* cs = reinterpret_cast<LPCRITICAL_SECTION>(&mtm->criticalSection);
			if (!TryEnterCriticalSection(cs)) {
				return;
			}
			RE::NiPointer<RE::TESObjectREFR> speaker = mtm->speaker.get();
			auto*                            dlg = mtm->lastSelectedDialogue;
			std::string                      choice;
			std::vector<std::string>         responses;
			if (dlg) {
				choice = dlg->topicText.c_str() ? dlg->topicText.c_str() : "";
				for (auto* r : dlg->responses) {
					if (r && r->text.c_str() && *r->text.c_str()) {
						responses.emplace_back(r->text.c_str());
					}
				}
			}
			LeaveCriticalSection(cs);

			if (!dlg || (dlg == g_lastDialogue && choice == g_lastDialogueText)) {
				return;
			}
			g_lastDialogue = dlg;
			g_lastDialogueText = choice;
			if (!choice.empty() && speaker) {
				json j{ { "to", Who(speaker.get()) }, { "text", choice } };
				Log().Push("player_dialogue", std::move(j));
			}
			for (auto& line : responses) {
				EmitHeard(speaker.get(), line, true);
			}
		}

		void PollAgents()
		{
			for (const auto& [agent, id] : Agents::Snapshot()) {
				auto*      actor = RE::TESForm::LookupByID<RE::Actor>(id);
				const bool loaded = actor && actor->Is3DLoaded() && actor->GetParentCell();
				auto [it, inserted] = g_agentLoaded.try_emplace(id, loaded);
				if (!inserted && it->second != loaded) {
					it->second = loaded;
					json j{ { "agent", agent } };
					if (actor) {
						Describe::Placement(j, actor, RE::PlayerCharacter::GetSingleton());
					}
					Log().Push(loaded ? "agent_loaded" : "agent_unloaded", std::move(j));
				}
			}
		}
	}

	Core::EventLog& Log()
	{
		static Core::EventLog log{ 1024 };
		return log;
	}

	void Install()
	{
		Log().gameHours = [] {
			auto* cal = RE::Calendar::GetSingleton();
			return cal ? static_cast<double>(cal->GetHoursPassed()) : -1.0;
		};
		auto* sinks = Sinks::Get();
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESCombatEvent>(sinks);
			holder->AddEventSink<RE::TESDeathEvent>(sinks);
			holder->AddEventSink<RE::TESHitEvent>(sinks);
			holder->AddEventSink<RE::TESContainerChangedEvent>(sinks);
			holder->AddEventSink<RE::TESActorLocationChangeEvent>(sinks);
			holder->AddEventSink<RE::TESQuestStageEvent>(sinks);
			holder->AddEventSink<RE::TESActivateEvent>(sinks);
			holder->AddEventSink<RE::TESSpellCastEvent>(sinks);
			holder->AddEventSink<RE::TESEnterBleedoutEvent>(sinks);
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(sinks);
		}
		if (auto* src = RE::LevelIncrease::GetEventSource()) {
			src->AddEventSink(sinks);
		}
		logger::info("event sinks registered");
	}

	void Tick(float)
	{
		static std::uint32_t frame = 0;
		++frame;
		FlushItems();
		if (frame % 4 == 0) {  // ~15 Hz is plenty for speech
			PollSubtitles();
			PollDialogue();
		}
		if (frame % 30 == 0) {
			PollAgents();
		}
	}

	void PlayerChat(const std::string& a_text, const std::string& a_source, const std::string& a_agent)
	{
		if (a_text.empty()) {
			return;
		}
		json j{ { "text", a_text }, { "source", a_source } };
		if (!a_agent.empty()) {
			j["agent"] = a_agent;
		}
		Log().Push("player_chat", std::move(j));
	}
}
