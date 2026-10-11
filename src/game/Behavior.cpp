#include "PCH.h"
#include "Behavior.h"
#include "Actions.h"
#include "Agents.h"
#include "Describe.h"
#include "Events.h"
#include "Papyrus.h"
#include "Targets.h"

#include "core/Text.h"
#include "net/Rpc.h"

namespace Game::Behavior
{
	namespace
	{
		using Net::RpcError;
		using Clock = std::chrono::steady_clock;
		constexpr float kU = Targets::kUnitsPerMeter;

		struct Orders
		{
			// persisted
			std::string style{ "default" };
			RE::FormID  originalStyle{ 0 };
			bool        haveOriginal{ false };
			bool        restraint{ false };
			bool        medic{ false };
			float       medicThreshold{ 0.4f };
			std::string follow{ "relaxed" };
			float       followDistanceM{ 2.f };
			std::string side{ "left" };
			bool        remount{ true };  // get back on the horse when the player rides and you are not
			RE::FormID  horse{ 0 };       // the last mount this body rode
			// runtime
			bool              offsetActive{ false };
			RE::FormID        offsetSpace{ 0 };      // worldspace (or interior cell) it was issued in
			Clock::time_point lastHold{};
			std::map<std::uint32_t, Clock::time_point> busy;  // running actions (id -> give-up time): no hold
			Clock::time_point nextHeal{};
			Clock::time_point lastRestrainEvent{};
			bool              wasRiding{ false };
			bool              remountHold{ false };  // told to dismount: wait for the player's next ride
			int               remountTries{ 0 };
			Clock::time_point nextRemount{};
		};

		std::recursive_mutex          g_lock;
		std::map<std::string, Orders> g_orders;
		std::map<RE::FormID, bool>    g_lowHealth;  // hysteresis for health_low

		struct Preset
		{
			const char* name;
			RE::FormID  id;  // vanilla Skyrim.esm combat styles (load index 00 is stable)
			const char* summary;
		};
		constexpr Preset kPresets[] = {
			{ "ranged", 0x0003BE1D, "archer: favours bows and keeps its distance (csHumanMissile)" },
			{ "magic", 0x0003BE1C, "mage: favours spells and keeps some distance (csHumanMagic)" },
			{ "spellsword", 0x001034FD, "mixes weapons and spells (csWESpellsword)" },
			{ "melee", 0x00068847, "front-line fighter (csHumanMeleeLvl4)" },
			{ "defensive", 0x0003DECE, "cautious, blocks and waits for openings (csHumanBoss1H)" },
			{ "berserker", 0x0003D2C8, "aggressive melee (csHumanBerserkerLvl1)" },
		};

		json PresetList()
		{
			json j = json::object();
			j["default"] = "the body's own combat style";
			for (auto& p : kPresets) {
				j[p.name] = p.summary;
			}
			return j;
		}

		json ToJson(const Orders& o)
		{
			return { { "style", o.style }, { "original_style", o.originalStyle }, { "have_original", o.haveOriginal },
				{ "restraint", o.restraint }, { "medic", o.medic }, { "medic_threshold", o.medicThreshold },
				{ "follow", o.follow }, { "follow_distance_m", o.followDistanceM }, { "side", o.side },
				{ "remount", o.remount }, { "horse", o.horse } };
		}

		Orders FromJson(const json& j)
		{
			Orders o;
			o.style = j.value("style", o.style);
			o.originalStyle = j.value("original_style", o.originalStyle);
			o.haveOriginal = j.value("have_original", o.haveOriginal);
			o.restraint = j.value("restraint", o.restraint);
			o.medic = j.value("medic", o.medic);
			o.medicThreshold = j.value("medic_threshold", o.medicThreshold);
			o.follow = j.value("follow", o.follow);
			o.followDistanceM = j.value("follow_distance_m", o.followDistanceM);
			o.side = j.value("side", o.side);
			o.remount = j.value("remount", o.remount);
			o.horse = j.value("horse", o.horse);
			return o;
		}

		float HealthFraction(RE::Actor* a)
		{
			const float max = a->GetActorValueMax(RE::ActorValue::kHealth);
			return max > 0.f ? std::clamp(a->AsActorValueOwner()->GetActorValue(RE::ActorValue::kHealth) / max, 0.f, 1.f) : 1.f;
		}

		// ------------------------------------------------------------------ combat style
		void ApplyStyle(RE::Actor* a_actor, Orders& o)
		{
			auto* base = a_actor->GetActorBase();
			if (!base) {
				return;
			}
			if (!o.haveOriginal) {
				o.originalStyle = base->combatStyle ? base->combatStyle->GetFormID() : 0;
				o.haveOriginal = true;
			}
			RE::TESCombatStyle* cs = nullptr;
			if (o.style == "default") {
				cs = o.originalStyle ? RE::TESForm::LookupByID<RE::TESCombatStyle>(o.originalStyle) : nullptr;
			} else {
				for (auto& p : kPresets) {
					if (o.style == p.name) {
						cs = RE::TESForm::LookupByID<RE::TESCombatStyle>(p.id);
					}
				}
				if (!cs) {
					logger::warn("behavior: combat style preset '{}' not found", o.style);
					return;
				}
			}
			base->combatStyle = cs;
			logger::info("behavior: {} combat style -> {}", a_actor->GetDisplayFullName(), o.style);
		}

		// ------------------------------------------------------------------ restraint
		bool Spare(RE::Actor* a_target)
		{
			if (!a_target || a_target->IsDead()) {
				return false;
			}
			if (a_target->IsPlayerRef() || a_target->IsPlayerTeammate() || a_target->IsGuard()) {
				return true;
			}
			// Unaggressive actors (townsfolk, wildlife that only flees) are not threats.
			return a_target->AsActorValueOwner()->GetActorValue(RE::ActorValue::kAggression) <= 0.f;
		}

		// ------------------------------------------------------------------ medic
		RE::SpellItem* BestHeal(RE::Actor* a_actor, float a_magicka, float& a_cost)
		{
			RE::SpellItem* best = nullptr;
			float          bestMag = 0.f;
			for (auto* s : Targets::KnownSpells(a_actor)) {
				if (!s || s->GetSpellType() != RE::MagicSystem::SpellType::kSpell) {
					continue;
				}
				for (auto* e : s->effects) {
					auto* mgef = e ? e->baseEffect : nullptr;
					if (!mgef || mgef->data.primaryAV != RE::ActorValue::kHealth || mgef->IsHostile() || mgef->IsDetrimental()) {
						continue;
					}
					using A = RE::EffectSetting::Archetype;
					const auto arch = mgef->GetArchetype();
					if (arch != A::kValueModifier && arch != A::kPeakValueModifier && arch != A::kDualValueModifier) {
						continue;
					}
					// must reach someone else: a targeted spell, or a self spell with an area (Grand Healing)
					if (s->GetDelivery() == RE::MagicSystem::Delivery::kSelf && e->effectItem.area == 0) {
						continue;
					}
					const float cost = s->CalculateMagickaCost(a_actor);
					if (cost <= a_magicka && e->GetMagnitude() > bestMag) {
						best = s;
						bestMag = e->GetMagnitude();
						a_cost = cost;
					}
				}
			}
			return best;
		}

		void Medic(const std::string& a_agent, RE::Actor* a_actor, Orders& o)
		{
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc || pc->IsDead() || HealthFraction(pc) >= o.medicThreshold || Clock::now() < o.nextHeal) {
				return;
			}
			o.nextHeal = Clock::now() + 6s;
			if (a_actor->GetPosition().GetDistance(pc->GetPosition()) / kU > 25.f) {
				return;
			}
			auto*      av = a_actor->AsActorValueOwner();
			float      cost = 0.f;
			auto*      spell = BestHeal(a_actor, av->GetActorValue(RE::ActorValue::kMagicka), cost);
			auto*      caster = a_actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
			if (!spell || !caster) {
				o.nextHeal = Clock::now() + 15s;  // nothing to cast; health_low still tells the agent
				return;
			}
			av->DamageActorValue(RE::ActorValue::kMagicka, cost);
			caster->CastSpellImmediate(spell, false, pc, 1.f, false, 0.f, a_actor);
			Events::Log().Push("healed", { { "agent", a_agent }, { "target", "player" }, { "spell", spell->GetName() }, { "magicka_cost", std::round(cost) } });
		}

		// ------------------------------------------------------------------ follow style
		// Close follow = no wandering off and no benches when the player stops. It used to hold a
		// KeepOffsetFromActor spot beside the player the whole time, but that engine call fixes the
		// follower's facing when issued: after the player turned, the follower kept that facing and
		// moonwalked (re-issuing did not reset it; only a clear and a fresh issue did), and tight
		// radii made it circle the spot. So while the player travels the normal follower AI walks
		// the body, facing forward, and the offset is held only once the player has stood still for 2 s: a radius
		// wide enough that the player turning on the spot (which swings the spot around them)
		// does not move the follower, so it stays by their side instead of sandboxing.
		void Hold(RE::Actor* a_actor, const Orders& o)
		{
			auto*       pc = RE::PlayerCharacter::GetSingleton();
			const float side = o.side == "right" ? 1.f : o.side == "behind" ? 0.f : -1.f;
			const float dist = std::max(0.5f, o.followDistanceM) * kU;
			const float x = side * 60.f;
			const float y = -dist;
			const float followR = 2.f * dist + 3.f * kU;
			const float catchUpR = followR + 600.f;
			Papyrus::CallMethod(a_actor, "Actor", "KeepOffsetFromActor",
				RE::MakeFunctionArguments(static_cast<RE::Actor*>(pc), float{ x }, float{ y }, 0.f, 0.f, 0.f, 0.f, float{ catchUpR }, float{ followR }));
		}

		void Release(RE::Actor* a_actor)
		{
			Papyrus::CallMethod(a_actor, "Actor", "ClearKeepOffsetFromActor", RE::MakeFunctionArguments());
		}

		RE::FormID SpaceOf(RE::TESObjectREFR* a_ref)
		{
			if (auto* ws = a_ref->GetWorldspace()) {
				return ws->GetFormID();
			}
			auto* cell = a_ref->GetParentCell();
			return cell ? cell->GetFormID() : 0;
		}

		void UpkeepFollow(RE::Actor* a_actor, Orders& o, bool a_playerResting)
		{
			auto*      pc = RE::PlayerCharacter::GetSingleton();
			const auto now = Clock::now();
			std::erase_if(o.busy, [&](const auto& kv) { return now >= kv.second; });  // actions that never reported back
			const bool waiting = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.f;
			// Only while the player rests, and not while the body is doing something on purpose (walking
			// somewhere, sitting, getting on a horse), waiting, fighting or riding: the offset
			// overrides all other movement.
			const bool hold = o.follow == "close" && pc && a_playerResting && o.busy.empty() && !waiting && !a_actor->IsInCombat() &&
			                  !pc->IsInCombat() && !pc->IsOnMount() && !a_actor->IsOnMount();
			if (!hold) {
				if (o.offsetActive) {
					Release(a_actor);  // the follower AI (or the action) takes over
					o.offsetActive = false;
				}
				return;
			}
			const auto space = SpaceOf(a_actor);
			if (!o.offsetActive || o.offsetSpace != space) {
				Hold(a_actor, o);
				o.offsetActive = true;
				o.offsetSpace = space;
				o.lastHold = now;
			}
		}

		// ------------------------------------------------------------------ horse
		json WhoJson(RE::Actor* a)
		{
			return { { "id", Core::FormatFormID(a->GetFormID()) }, { "name", Game::Describe::Name(a) } };
		}

		bool Near(RE::Actor* a, RE::Actor* b, float a_m)
		{
			if (a->GetWorldspace() ? a->GetWorldspace() != b->GetWorldspace() : a->GetParentCell() != b->GetParentCell()) {
				return false;
			}
			return a->GetPosition().GetDistance(b->GetPosition()) / kU <= a_m;
		}

		// The remembered horse, else the nearest mount within 80 m owned by the body (or its faction).
		RE::Actor* LocateHorse(RE::Actor* a_actor, Orders& o)
		{
			if (o.horse) {
				auto* h = RE::TESForm::LookupByID<RE::Actor>(o.horse);
				if (h && Targets::Rideable(h, a_actor) && Near(a_actor, h, 100.f)) {
					return h;
				}
			}
			for (auto* ref : Targets::InRange(a_actor, 80.f * kU)) {
				auto* h = ref->As<RE::Actor>();
				if (h && h != a_actor && Targets::Rideable(h, a_actor) &&
					Targets::OwnershipFor(h, a_actor) == Targets::Ownership::kMine) {
					o.horse = h->GetFormID();
					return h;
				}
			}
			return nullptr;
		}

		struct RemountRequest
		{
			std::string agent;
			RE::Actor*  actor;
			RE::Actor*  horse;
		};

		void UpkeepMount(const std::string& a_agent, RE::Actor* a_actor, Orders& o, std::vector<RemountRequest>& a_out)
		{
			auto*      mount = Targets::MountOf(a_actor);
			const bool riding = mount != nullptr;
			if (riding != o.wasRiding) {
				o.wasRiding = riding;
				json j{ { "who", { { "id", Core::FormatFormID(a_actor->GetFormID()) }, { "name", Game::Describe::Name(a_actor) }, { "agent", a_agent } } },
					{ "state", riding ? "mounted" : "dismounted" } };
				if (riding) {
					j["horse"] = WhoJson(mount);
				}
				Events::Log().Push("mount", std::move(j));
			}
			if (riding) {
				o.horse = mount->GetFormID();
				o.remountTries = 0;
				return;
			}
			auto* pc = RE::PlayerCharacter::GetSingleton();
			if (!pc || !pc->IsOnMount()) {
				o.remountHold = false;  // the next ride starts fresh
				o.remountTries = 0;
				return;
			}
			const bool waiting = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.f;
			if (!o.remount || o.remountHold || waiting || o.remountTries >= 3 || Clock::now() < o.nextRemount ||
				a_actor->IsInCombat() || pc->IsInCombat()) {
				return;
			}
			auto* horse = LocateHorse(a_actor, o);
			if (!horse || !Near(a_actor, horse, 60.f)) {
				return;
			}
			o.nextRemount = Clock::now() + 20s;
			++o.remountTries;
			logger::info("behavior: {} remounting {} (try {})", a_actor->GetDisplayFullName(), Game::Describe::Name(horse), o.remountTries);
			a_out.push_back({ a_agent, a_actor, horse });  // started after the lock is released
		}
	}

	RE::Actor* FindHorse(const std::string& a_agent, RE::Actor* a_actor)
	{
		std::lock_guard lock{ g_lock };
		return LocateHorse(a_actor, g_orders[a_agent]);
	}

	void OnMountOrder(const std::string& a_agent, RE::Actor* a_horse)
	{
		std::lock_guard lock{ g_lock };
		auto&           o = g_orders[a_agent];
		if (a_horse) {
			o.horse = a_horse->GetFormID();
		}
		o.remountHold = false;
	}

	void BeginAction(const std::string& a_agent, RE::Actor* a_actor, std::uint32_t a_id, std::chrono::seconds a_max)
	{
		std::lock_guard lock{ g_lock };
		auto&           o = g_orders[a_agent];
		o.busy[a_id] = Clock::now() + a_max;
		if (o.offsetActive && a_actor) {
			Release(a_actor);  // before the action's own pathing starts, or the offset overrides it
			o.offsetActive = false;
		}
	}

	void EndAction(const std::string& a_agent, std::uint32_t a_id)
	{
		std::lock_guard lock{ g_lock };
		if (auto it = g_orders.find(a_agent); it != g_orders.end()) {
			it->second.busy.erase(a_id);
		}
	}

	void OnDismountOrder(const std::string& a_agent)
	{
		std::lock_guard lock{ g_lock };
		g_orders[a_agent].remountHold = true;
	}

	json Describe(const std::string& a_agent)
	{
		std::lock_guard lock{ g_lock };
		const auto      it = g_orders.find(a_agent);
		const Orders    o = it != g_orders.end() ? it->second : Orders{};
		return { { "combat", { { "style", o.style }, { "restraint", o.restraint }, { "medic", o.medic }, { "medic_threshold", o.medicThreshold } } },
			{ "follow", { { "mode", o.follow }, { "distance_m", o.followDistanceM }, { "side", o.side }, { "remount", o.remount } } } };
	}

	json SetCombat(const json& p)
	{
		const auto agent = Agents::AgentOf(p);
		auto*      actor = Agents::Require(p, false);
		{
			std::lock_guard lock{ g_lock };
			auto&           o = g_orders[agent];
			if (const auto style = Net::OptString(p, "style"); !style.empty()) {
				const auto s = Core::Lower(style);
				const bool known = s == "default" || std::ranges::any_of(kPresets, [&](const Preset& x) { return s == x.name; });
				if (!known) {
					throw RpcError("bad_request", "unknown style '" + style + "'; options: " + PresetList().dump());
				}
				o.style = s;
				ApplyStyle(actor, o);
			}
			if (p.contains("restraint")) {
				o.restraint = Net::OptBool(p, "restraint", o.restraint);
			}
			if (p.contains("medic")) {
				o.medic = Net::OptBool(p, "medic", o.medic);
			}
			if (p.contains("medic_threshold")) {
				o.medicThreshold = static_cast<float>(std::clamp(Net::OptNumber(p, "medic_threshold", o.medicThreshold), 0.1, 0.9));
			}
		}
		auto j = Describe(agent);
		j["style_options"] = PresetList();
		j["note"] = "style changes take effect from the next fight; restraint and medic apply immediately";
		return j;
	}

	json SetFollowStyle(const json& p)
	{
		const auto agent = Agents::AgentOf(p);
		auto*      actor = Agents::Require(p, false);
		{
			std::lock_guard lock{ g_lock };
			auto&           o = g_orders[agent];
			const auto      mode = Core::Lower(Net::OptString(p, "mode", o.follow));
			if (mode != "close" && mode != "relaxed") {
				throw RpcError("bad_request", "mode is 'close' (stay beside the player) or 'relaxed' (normal follower behaviour)");
			}
			const auto side = Core::Lower(Net::OptString(p, "side", o.side));
			if (side != "left" && side != "right" && side != "behind") {
				throw RpcError("bad_request", "side is left, right or behind");
			}
			o.follow = mode;
			o.side = side;
			o.remount = Net::OptBool(p, "remount", o.remount);
			o.followDistanceM = static_cast<float>(std::clamp(Net::OptNumber(p, "distance_m", o.followDistanceM), 1.0, 8.0));
			if (mode == "relaxed" && o.offsetActive) {
				Release(actor);
			}
			o.offsetActive = false;  // (re)apply on the next tick
		}
		return Describe(agent);
	}

	bool Restrains(const std::string& a_agent, RE::Actor* a_target)
	{
		std::lock_guard lock{ g_lock };
		const auto      it = g_orders.find(a_agent);
		return it != g_orders.end() && it->second.restraint && Spare(a_target);
	}

	void OnWaitChanged(const std::string& a_agent, bool)
	{
		std::lock_guard lock{ g_lock };
		if (auto it = g_orders.find(a_agent); it != g_orders.end()) {
			it->second.lastHold = {};  // re-evaluated on the next follow tick
		}
	}

	void Tick(float)
	{
		static Clock::time_point nextFast{}, nextSlow{}, nextFollow{};
		const auto               now = Clock::now();
		const bool               fast = now >= nextFast;    // 4 Hz: restraint
		const bool               slow = now >= nextSlow;    // 2 Hz: medic, health
		const bool               follow = now >= nextFollow;  // 2 Hz: follow upkeep
		if (!fast && !slow && !follow) {
			return;
		}
		if (fast) {
			nextFast = now + 250ms;
		}
		if (slow) {
			nextSlow = now + 500ms;
		}
		if (follow) {
			nextFollow = now + 500ms;
		}

		std::vector<std::pair<std::string, RE::Actor*>> bodies;
		for (const auto& [agent, id] : Agents::Snapshot()) {
			if (auto* a = RE::TESForm::LookupByID<RE::Actor>(id); a && a->Is3DLoaded() && !a->IsDead()) {
				bodies.emplace_back(agent, a);
			}
		}

		if (slow) {  // health_low for the player and every body, once per dip
			auto check = [](RE::Actor* a, const json& who) {
				const float f = HealthFraction(a);
				auto&       low = g_lowHealth[a->GetFormID()];
				if (!low && f < 0.35f && !a->IsDead()) {
					low = true;
					Events::Log().Push("health_low", { { "who", who }, { "health", std::round(f * 100.f) / 100.f } });
				} else if (low && f > 0.6f) {
					low = false;
				}
			};
			if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
				check(pc, json{ { "id", "0x00000014" }, { "name", Game::Describe::Name(pc) }, { "is_player", true } });
			}
			for (auto& [agent, a] : bodies) {
				check(a, json{ { "id", Core::FormatFormID(a->GetFormID()) }, { "name", Game::Describe::Name(a) }, { "agent", agent } });
			}
		}

		if (follow) {  // the player getting on or off a horse
			static bool playerRiding = false;
			auto*       pc = RE::PlayerCharacter::GetSingleton();
			auto*       pm = pc ? Targets::MountOf(pc) : nullptr;
			if ((pm != nullptr) != playerRiding) {
				playerRiding = pm != nullptr;
				json j{ { "who", { { "id", "0x00000014" }, { "name", Game::Describe::Name(pc) }, { "is_player", true } } },
					{ "state", playerRiding ? "mounted" : "dismounted" } };
				if (pm) {
					j["horse"] = WhoJson(pm);
				}
				Events::Log().Push("mount", std::move(j));
			}
		}

		// Is the player resting? (still for 2 s; moving again above 0.8 m/s; in between: unchanged)
		static RE::NiPoint3      pcLast{};
		static Clock::time_point pcLastAt{};
		static Clock::time_point pcStillSince{};
		static bool              pcResting = false;
		if (follow) {
			if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
				const auto  pos = pc->GetPosition();
				const float dt = pcLastAt == Clock::time_point{} ? 0.5f : std::max(0.05f, std::chrono::duration<float>(now - pcLastAt).count());
				const float speed = std::hypot(pos.x - pcLast.x, pos.y - pcLast.y) / kU / dt;  // m/s
				pcLast = pos;
				pcLastAt = now;
				if (speed < 0.4f) {
					if (pcStillSince == Clock::time_point{}) {
						pcStillSince = now;
					}
				} else {
					pcStillSince = {};
				}
				if (speed > 0.8f) {
					pcResting = false;
				} else if (pcStillSince != Clock::time_point{} && now - pcStillSince >= 2s) {
					pcResting = true;
				}
			}
		}

		std::vector<RemountRequest> remounts;
		{
			std::lock_guard lock{ g_lock };
			for (auto& [agent, a] : bodies) {
				auto& o = g_orders[agent];
				if (fast && o.restraint && a->IsInCombat()) {
					auto target = a->GetActorRuntimeData().currentCombatTarget.get();
					if (target && Spare(target.get())) {
						a->StopCombat();
						if (now - o.lastRestrainEvent > 10s) {
							o.lastRestrainEvent = now;
							Events::Log().Push("restrained", { { "agent", agent }, { "target", Game::Describe::Name(target.get()) },
																 { "reason", target->IsGuard() ? "guard" : target->IsPlayerTeammate() ? "ally" : "not hostile" } });
						}
					}
				}
				if (slow && o.medic) {
					Medic(agent, a, o);
				}
				if (follow) {
					UpkeepFollow(a, o, pcResting);
					UpkeepMount(agent, a, o, remounts);
				}
			}
		}
		for (auto& r : remounts) {
			Actions::ApproachAndActivate(r.actor, r.agent, r.horse, "remount");
		}
	}

	void OnGameLoaded()
	{
		std::lock_guard lock{ g_lock };
		for (auto& [agent, o] : g_orders) {
			o.offsetActive = false;
			o.busy.clear();
			o.wasRiding = false;
			o.remountTries = 0;
			if (o.style != "default") {
				if (auto* a = Agents::Get(agent)) {
					ApplyStyle(a, o);  // base forms revert to their plugin values on load
				}
			}
		}
	}

	void Save(SKSE::SerializationInterface* a_intfc)
	{
		std::lock_guard lock{ g_lock };
		if (!a_intfc->OpenRecord(kRecord, kVersion)) {
			return;
		}
		const auto count = static_cast<std::uint32_t>(g_orders.size());
		a_intfc->WriteRecordData(count);
		for (const auto& [agent, o] : g_orders) {
			const auto blob = ToJson(o).dump();
			const auto alen = static_cast<std::uint32_t>(agent.size());
			const auto blen = static_cast<std::uint32_t>(blob.size());
			a_intfc->WriteRecordData(alen);
			a_intfc->WriteRecordData(agent.data(), alen);
			a_intfc->WriteRecordData(blen);
			a_intfc->WriteRecordData(blob.data(), blen);
		}
	}

	void Load(SKSE::SerializationInterface* a_intfc, std::uint32_t a_version)
	{
		if (a_version != kVersion) {
			return;
		}
		std::map<std::string, Orders> loaded;
		std::uint32_t                 count = 0;
		a_intfc->ReadRecordData(count);
		for (std::uint32_t i = 0; i < count && i < 256; ++i) {
			std::uint32_t alen = 0, blen = 0;
			a_intfc->ReadRecordData(alen);
			if (alen > 256) {
				return;
			}
			std::string agent(alen, '\0');
			a_intfc->ReadRecordData(agent.data(), alen);
			a_intfc->ReadRecordData(blen);
			if (blen > 65536) {
				return;
			}
			std::string blob(blen, '\0');
			a_intfc->ReadRecordData(blob.data(), blen);
			const auto j = json::parse(blob, nullptr, false);
			if (!j.is_discarded()) {
				auto o = FromJson(j);
				if (o.haveOriginal && o.originalStyle) {
					RE::FormID resolved = 0;
					o.originalStyle = a_intfc->ResolveFormID(o.originalStyle, resolved) ? resolved : 0;
				}
				if (o.horse) {
					RE::FormID resolved = 0;
					o.horse = a_intfc->ResolveFormID(o.horse, resolved) ? resolved : 0;
				}
				loaded[agent] = std::move(o);
			}
		}
		std::lock_guard lock{ g_lock };
		g_orders = std::move(loaded);
	}

	void Revert()
	{
		std::lock_guard lock{ g_lock };
		g_orders.clear();
		g_lowHealth.clear();
	}
}
