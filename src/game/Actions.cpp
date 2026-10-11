#include "PCH.h"
#include "Actions.h"
#include "Agents.h"
#include "Behavior.h"
#include "Body.h"
#include "Describe.h"
#include "Events.h"
#include "MainThread.h"
#include "Papyrus.h"
#include "Targets.h"
#include "Voice.h"

#include "Settings.h"
#include "core/Text.h"
#include "net/Rpc.h"

namespace Game::Actions
{
	namespace
	{
		using Net::RpcError;
		constexpr float kU = Targets::kUnitsPerMeter;
		constexpr float kReachM = 4.5f;      // activate / pick up
		constexpr float kHandOverM = 8.f;    // give
		constexpr float kMaxTravelM = 250.f; // move_to position sanity bound

		std::atomic<std::uint32_t>                   g_nextAction{ 100000 };  // disjoint from mod action ids
		std::map<std::string, RE::ObjectRefHandle>   g_markers;              // agent -> travel marker

		RE::Actor* Body(const json& p)
		{
			if (!Settings::Get().actionsEnabled) {
				throw RpcError("disabled", "actions are disabled ([Actions] Enabled)");
			}
			MainThread::RequireInGame();
			auto* actor = Agents::Require(p, true);
			if (actor->IsDead()) {
				throw RpcError("failed", "your body is dead");
			}
			return actor;
		}

		float DistM(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			return a->GetPosition().GetDistance(b->GetPosition()) / kU;
		}

		void RequireReach(RE::Actor* a_actor, RE::TESObjectREFR* a_target, float a_reachM)
		{
			const float d = DistM(a_actor, a_target);
			if (d > a_reachM) {
				throw RpcError("failed", std::format("{} is {:.1f} m away; get within {:.0f} m first (move_to)",
											 Describe::Name(a_target), d, a_reachM));
			}
		}

		json Ok(json a_extra = json::object())
		{
			a_extra["ok"] = true;
			return a_extra;
		}

		RE::TESObjectREFR* TravelMarker(const std::string& a_agent, RE::Actor* a_actor)
		{
			if (auto it = g_markers.find(a_agent); it != g_markers.end()) {
				if (auto m = it->second.get(); m && !m->IsDeleted() && m->GetParentCell() == a_actor->GetParentCell()) {
					return m.get();
				}
			}
			auto* xmarker = RE::TESForm::LookupByID<RE::TESBoundObject>(0x3B);  // XMarker
			if (!xmarker) {
				throw RpcError("internal", "XMarker base form missing");
			}
			auto marker = a_actor->PlaceObjectAtMe(xmarker, false);
			if (!marker) {
				throw RpcError("failed", "could not place a travel marker");
			}
			g_markers[a_agent] = marker->GetHandle();
			return marker.get();
		}

		void SetWaiting(RE::Actor* a_actor, bool a_wait)
		{
			a_actor->AsActorValueOwner()->SetActorValue(RE::ActorValue::kWaitingForPlayer, a_wait ? 1.f : 0.f);
			a_actor->EvaluatePackage(true, false);
		}

		void Finished(std::uint32_t a_id, const std::string& a_agent, const char* a_action, bool a_ok, json a_extra = json::object())
		{
			a_extra["action_id"] = a_id;
			a_extra["agent"] = a_agent;
			a_extra["action"] = a_action;
			a_extra["ok"] = a_ok;
			Behavior::EndAction(a_agent, a_id);
			Events::Log().Push("action_finished", std::move(a_extra));
		}

		// ------------------------------------------------------------------ ownership, reach
		constexpr float kApproachMaxM = 80.f;  // activate walks over first from up to this far
		constexpr float kMountReachM = 6.f;    // a horse is long: centre to centre exceeds 4.5 m beside it
		constexpr int   kApproachTries = 3;    // a wandering horse gets chased this many times

		bool IsMount(RE::TESObjectREFR* a_ref)
		{
			auto* a = a_ref ? a_ref->As<RE::Actor>() : nullptr;
			return a && a->IsAMount();
		}

		float ReachFor(RE::TESObjectREFR* a_target)
		{
			return IsMount(a_target) ? kMountReachM : kReachM;
		}

		// The engine's crime test is the player's; the agent's own things (its horse, its bedroll) are fine.
		void RequireNotCrime(RE::Actor* a_actor, RE::TESObjectREFR* a_target, bool a_allow, const char* a_doing, const char* a_flag)
		{
			if (a_allow || Targets::OwnershipFor(a_target, a_actor) != Targets::Ownership::kOther) {
				return;
			}
			const auto owner = Targets::OwnerName(a_target);
			throw RpcError("failed", std::format("{} belongs to {}; {} would be a crime (pass {} to insist)", Describe::Name(a_target),
										 owner.empty() ? "someone" : owner, a_doing, a_flag));
		}

		bool SameSpace(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
		{
			auto* wa = a->GetWorldspace();
			return wa ? wa == b->GetWorldspace() : a->GetParentCell() == b->GetParentCell();
		}

		// ------------------------------------------------------------------ approach, then activate
		void ActivateAndReport(std::uint32_t a_id, RE::Actor* a_actor, RE::TESObjectREFR* a_target, const std::string& a_agent, const std::string& a_action);

		void Approach(std::uint32_t a_id, RE::FormID a_actorId, RE::ObjectRefHandle a_target, std::string a_agent, std::string a_action, int a_try)
		{
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
			auto  target = a_target.get();
			if (!actor || actor->IsDead() || !target || target->IsDisabled()) {
				Finished(a_id, a_agent, a_action.c_str(), false, { { "reason", "it (or your body) is gone" } });
				return;
			}
			const bool run = DistM(actor, target.get()) > 12.f;
			auto*      args = RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(target.get()), run ? 1.0f : 0.0f);
			const bool ok = Papyrus::CallMethod(actor, "Actor", "PathToReference", args,
				[a_id, a_actorId, a_target, a_agent, a_action, a_try](const RE::BSScript::Variable&) {
					SKSE::GetTaskInterface()->AddTask([=]() {
						auto* a = RE::TESForm::LookupByID<RE::Actor>(a_actorId);
						auto  t = a_target.get();
						if (!a || a->IsDead() || !t || t->IsDisabled()) {
							Finished(a_id, a_agent, a_action.c_str(), false, { { "reason", "it (or your body) is gone" } });
							return;
						}
						const float d = DistM(a, t.get());
						if (d <= ReachFor(t.get())) {
							ActivateAndReport(a_id, a, t.get(), a_agent, a_action);
						} else if (a_try < kApproachTries && d <= kApproachMaxM && SameSpace(a, t.get())) {
							Approach(a_id, a_actorId, a_target, a_agent, a_action, a_try + 1);  // it moved: go after it again
						} else {
							Finished(a_id, a_agent, a_action.c_str(), false, { { "target", Describe::Name(t.get()) },
								{ "distance_m", std::round(d * 10.f) / 10.f }, { "reason", "could not get close enough" } });
						}
					});
				});
			if (!ok) {
				Finished(a_id, a_agent, a_action.c_str(), false, { { "reason", "could not start walking there" } });
			}
		}

		void ActivateAndReport(std::uint32_t a_id, RE::Actor* a_actor, RE::TESObjectREFR* a_target, const std::string& a_agent, const std::string& a_action)
		{
			const auto name = Describe::Name(a_target);
			const bool mount = IsMount(a_target);
			const bool ok = a_target->ActivateRef(a_actor, 0, nullptr, 1, false);
			if (!mount || !ok) {
				json extra{ { "target", name }, { "kind", Describe::Kind(a_target) } };
				if (mount) {
					extra["mounted"] = false;
					extra["reason"] = "it would not let you on";
				}
				Finished(a_id, a_agent, a_action.c_str(), ok, std::move(extra));
				return;
			}
			Behavior::OnMountOrder(a_agent, a_target->As<RE::Actor>());
			// Climbing on takes a couple of seconds of animation; report once it is done.
			const auto actorId = a_actor->GetFormID();
			const auto mountId = a_target->GetFormID();
			std::thread([=]() {
				std::this_thread::sleep_for(3s);
				SKSE::GetTaskInterface()->AddTask([=]() {
					auto*      a = RE::TESForm::LookupByID<RE::Actor>(actorId);
					auto*      m = a ? Targets::MountOf(a) : nullptr;
					const bool on = m && m->GetFormID() == mountId;
					json       extra{ { "target", name }, { "kind", "actor" }, { "mounted", on } };
					if (!on) {
						extra["reason"] = "you did not end up on it (blocked, or the animation was interrupted); try again";
					}
					Finished(a_id, a_agent, a_action.c_str(), on, std::move(extra));
				});
			}).detach();
		}
	}

	std::uint32_t ApproachAndActivate(RE::Actor* a_actor, const std::string& a_agent, RE::TESObjectREFR* a_target, const char* a_action)
	{
		const auto id = g_nextAction++;
		Behavior::BeginAction(a_agent, a_actor, id);
		if (DistM(a_actor, a_target) <= ReachFor(a_target)) {
			ActivateAndReport(id, a_actor, a_target, a_agent, a_action);
		} else {
			Approach(id, a_actor->GetFormID(), a_target->GetHandle(), a_agent, a_action, 1);
		}
		return id;
	}

	json MoveTo(const json& p)
	{
		auto*      actor = Body(p);
		const auto agent = Agents::AgentOf(p);
		const bool run = Net::OptBool(p, "run", false);
		const bool stay = Net::OptBool(p, "stay", false);

		RE::TESObjectREFR* target = nullptr;
		if (auto it = p.find("position"); it != p.end() && !it->is_null()) {
			if (!it->is_array() || it->size() != 3 || !(*it)[0].is_number() || !(*it)[1].is_number() || !(*it)[2].is_number()) {
				throw RpcError("bad_request", "position must be [x, y, z]");
			}
			const RE::NiPoint3 pos{ (*it)[0].get<float>(), (*it)[1].get<float>(), (*it)[2].get<float>() };
			if (pos.GetDistance(actor->GetPosition()) / kU > kMaxTravelM) {
				throw RpcError("failed", "that position is too far away to path to directly");
			}
			target = TravelMarker(agent, actor);
			target->SetPosition(pos);
		} else {
			target = Targets::Ref(Net::RequireString(p, "target"), actor);
		}
		if (target == actor) {
			throw RpcError("bad_request", "you are already there");
		}

		// Pathing to a load door makes the AI go *through* it. Stop on this side instead: the linked
		// door's teleport point is exactly where someone coming through would land, in front of it.
		std::string doorNote;
		if (auto* base = target->GetBaseObject(); base && base->Is(RE::FormType::Door)) {
			if (auto linked = target->extraList.GetTeleportLinkedDoor().get()) {
				auto* tp = linked->extraList.GetByType<RE::ExtraTeleport>();
				if (tp && tp->teleportData && tp->teleportData->position.GetDistance(target->GetPosition()) < 15.f * kU &&
					target->GetParentCell() == actor->GetParentCell()) {
					const auto doorName = Describe::Name(target);
					auto*      marker = TravelMarker(agent, actor);
					marker->SetPosition(tp->teleportData->position);
					target = marker;
					doorNote = "stopping in front of the door to " + (doorName.empty() ? std::string("elsewhere") : doorName) +
					           "; activate it to go through";
				}
			}
		}

		Body::Cancel(agent);
		const auto id = g_nextAction++;
		const auto targetName = Describe::Name(target);
		Behavior::BeginAction(agent, actor, id);  // close-follow lets go first, or it would override the walk
		auto* args = RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(target), run ? 1.0f : 0.0f);
		const RE::FormID actorId = actor->GetFormID();
		const bool dispatched = Papyrus::CallMethod(actor, "Actor", "PathToReference", args,
			[id, agent, targetName, stay, actorId](const RE::BSScript::Variable& a_result) {
				const bool ok = a_result.IsBool() ? a_result.GetBool() : true;
				SKSE::GetTaskInterface()->AddTask([=]() {
					if (stay) {  // wait before the action ends, so close-follow does not grab the body in between
						if (auto* a = RE::TESForm::LookupByID<RE::Actor>(actorId)) {
							SetWaiting(a, true);
						}
					}
					Finished(id, agent, "move_to", ok, { { "target", targetName }, { "arrived", ok } });
				});
			});
		if (!dispatched) {
			Behavior::EndAction(agent, id);
			throw RpcError("failed", "could not start pathing (Papyrus dispatch failed)");
		}
		json out{ { "action_id", id }, { "target", Describe::Brief(target, actor) } };
		if (!doorNote.empty()) {
			out["door"] = doorNote;
		}
		if (!stay) {
			out["note"] = "followers resume following the player after arriving unless stay=true";
		}
		return out;
	}

	json Follow(const json& p)
	{
		auto* actor = Body(p);
		Body::Cancel(Agents::AgentOf(p));
		SetWaiting(actor, false);
		Behavior::OnWaitChanged(Agents::AgentOf(p), false);
		return Ok();
	}

	json WaitHere(const json& p)
	{
		auto* actor = Body(p);
		SetWaiting(actor, true);
		Behavior::OnWaitChanged(Agents::AgentOf(p), true);
		return Ok();
	}

	json Activate(const json& p)
	{
		auto*      actor = Body(p);
		const auto agent = Agents::AgentOf(p);
		auto*      target = Targets::Ref(Net::RequireString(p, "target"), actor);
		if (target == actor) {
			throw RpcError("bad_request", "you cannot activate yourself");
		}
		RequireNotCrime(actor, target, Net::OptBool(p, "allow_crime", false), "using it", "allow_crime");
		const float d = DistM(actor, target);
		const bool  mount = IsMount(target);
		if (mount && !Targets::Rideable(target->As<RE::Actor>(), actor)) {
			throw RpcError("failed", Describe::Name(target) + " cannot be ridden right now (someone is on it, or it is down)");
		}
		if (d > ReachFor(target)) {
			if (!Net::OptBool(p, "approach", true) || d > kApproachMaxM || !SameSpace(actor, target)) {
				RequireReach(actor, target, ReachFor(target));  // throws, suggesting move_to
			}
			Body::Cancel(agent);
			const auto id = ApproachAndActivate(actor, agent, target, "activate");
			return { { "action_id", id }, { "approaching", true }, { "distance_m", std::round(d * 10.f) / 10.f },
				{ "target", Describe::Brief(target, actor) }, { "note", "walking over first; action_finished says how it went" } };
		}
		if (mount) {  // climbing on takes a moment: report through action_finished
			Body::Cancel(agent);
			const auto id = ApproachAndActivate(actor, agent, target, "activate");
			return { { "action_id", id }, { "target", Describe::Name(target) }, { "kind", "actor" },
				{ "note", "mounting; action_finished says whether you are on" } };
		}
		const bool ok = target->ActivateRef(actor, 0, nullptr, 1, false);
		return json{ { "ok", ok }, { "target", Describe::Name(target) }, { "kind", Describe::Kind(target) } };
	}

	json Mount(const json& p)
	{
		auto*      actor = Body(p);
		const auto agent = Agents::AgentOf(p);
		if (auto* m = Targets::MountOf(actor)) {
			return Ok({ { "already_riding", Describe::Name(m) } });
		}
		RE::Actor* horse = nullptr;
		if (const auto spec = Net::OptString(p, "target"); !spec.empty()) {
			horse = Targets::Actor(spec, actor);
			if (!horse->IsAMount()) {
				throw RpcError("failed", Describe::Name(horse) + " is not something you can ride");
			}
		} else {
			horse = Behavior::FindHorse(agent, actor);
			if (!horse) {
				throw RpcError("not_found", "no horse of yours nearby (none remembered, none owned by you within 80 m); pass a target");
			}
		}
		if (!Targets::Rideable(horse, actor)) {
			throw RpcError("failed", Describe::Name(horse) + " cannot be ridden right now (someone is on it, or it is down)");
		}
		RequireNotCrime(actor, horse, Net::OptBool(p, "allow_crime", false), "riding it", "allow_crime");
		const float d = DistM(actor, horse);
		if (d > kApproachMaxM || !SameSpace(actor, horse)) {
			throw RpcError("failed", std::format("{} is {:.0f} m away; too far to walk over to", Describe::Name(horse), d));
		}
		Body::Cancel(agent);
		const auto id = ApproachAndActivate(actor, agent, horse, "mount");
		return { { "action_id", id }, { "horse", Describe::Brief(horse, actor) },
			{ "note", d > kMountReachM ? "walking over, then climbing on; action_finished says when you are on" :
			                             "climbing on; action_finished says when you are on" } };
	}

	json Dismount(const json& p)
	{
		auto*      actor = Body(p);
		const auto agent = Agents::AgentOf(p);
		auto*      horse = Targets::MountOf(actor);
		if (!horse) {
			return { { "ok", false }, { "note", "you are not riding" } };
		}
		const auto name = Describe::Name(horse);
		Behavior::OnDismountOrder(agent);
		if (!Papyrus::CallMethod(actor, "Actor", "Dismount", RE::MakeFunctionArguments())) {
			throw RpcError("failed", "could not dismount (Papyrus dispatch failed)");
		}
		return Ok({ { "horse", name }, { "note", "remount (standing order) waits until the player next gets off and on a horse" } });
	}

	json PickUp(const json& p)
	{
		auto* actor = Body(p);
		auto* target = Targets::Ref(Net::RequireString(p, "target"), actor);
		if (Describe::Kind(target) != "item") {
			throw RpcError("failed", Describe::Name(target) + " is not something you can pick up");
		}
		RequireReach(actor, target, kReachM);
		RequireNotCrime(actor, target, Net::OptBool(p, "allow_theft", false), "taking it", "allow_theft");
		const auto available = std::max<std::int32_t>(1, target->extraList.GetCount());
		const auto count = std::clamp<std::int32_t>(static_cast<std::int32_t>(Net::OptNumber(p, "count", available)), 1, available);
		const auto name = Describe::Name(target);
		actor->PickUpObject(target, count, false, true);
		return Ok({ { "item", name }, { "count", count } });
	}

	json Give(const json& p)
	{
		auto* actor = Body(p);
		auto  item = Targets::Item(actor, Net::RequireString(p, "item"));
		auto* to = Targets::Ref(Net::OptString(p, "to", "player"), actor);
		if (to == actor) {
			throw RpcError("bad_request", "you cannot give yourself an item");
		}
		RequireReach(actor, to, kHandOverM);
		const auto count = std::clamp<std::int32_t>(static_cast<std::int32_t>(Net::OptNumber(p, "count", 1)), 1, item.count);
		actor->RemoveItem(item.object, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, to);
		return Ok({ { "item", item.object->GetName() }, { "count", count }, { "to", Describe::Name(to) } });
	}

	json UseItem(const json& p)
	{
		auto*      actor = Body(p);
		const auto spec = Net::RequireString(p, "item");
		const auto hand = Core::Lower(Net::OptString(p, "hand"));  // "", "left", "right"
		if (!hand.empty() && hand != "left" && hand != "right") {
			throw RpcError("bad_request", "hand is left or right");
		}
		auto* em = RE::ActorEquipManager::GetSingleton();
		auto* dom = RE::BGSDefaultObjectManager::GetSingleton();
		auto* slot = hand.empty() ? nullptr : dom->GetObject<RE::BGSEquipSlot>(hand == "left" ? RE::DEFAULT_OBJECT::kLeftHandEquip : RE::DEFAULT_OBJECT::kRightHandEquip);
		try {
			auto       item = Targets::Item(actor, spec);
			const auto kind = Describe::ItemKind(item.object);
			if (kind == "poison") {
				throw RpcError("failed", "applying poisons is not supported");
			}
			em->EquipObject(actor, item.object, nullptr, 1, kind == "weapon" ? slot : nullptr, true, false, true, false);
			const bool consumed = kind == "potion" || kind == "food" || kind == "ingredient";
			json out{ { "item", item.object->GetName() }, { "kind", kind }, { "consumed", consumed } };
			if (kind == "weapon" && slot) {
				out["hand"] = hand;
			}
			return Ok(std::move(out));
		} catch (const RpcError& e) {
			if (e.code != "not_found") {
				throw;
			}
		}
		auto* spell = Targets::Spell(actor, spec);  // equip a known spell instead
		if (!slot) {
			slot = dom->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECT::kRightHandEquip);
		}
		em->EquipSpell(actor, spell, slot);
		return Ok({ { "spell", spell->GetName() }, { "hand", hand.empty() ? "right" : hand } });
	}

	json Unequip(const json& p)
	{
		auto* actor = Body(p);
		auto  item = Targets::Item(actor, Net::RequireString(p, "item"));
		if (!item.worn) {
			throw RpcError("failed", std::string(item.object->GetName()) + " is not equipped");
		}
		RE::ActorEquipManager::GetSingleton()->UnequipObject(actor, item.object, nullptr, 1, nullptr, true, false, true, false, nullptr);
		return Ok({ { "item", item.object->GetName() } });
	}

	json Cast(const json& p)
	{
		auto* actor = Body(p);
		auto* spell = Targets::Spell(actor, Net::RequireString(p, "spell"));
		RE::TESObjectREFR* target = nullptr;
		if (spell->GetDelivery() == RE::MagicSystem::Delivery::kSelf) {
			target = actor;
		} else if (const auto t = Net::OptString(p, "target"); !t.empty()) {
			target = Targets::Ref(t, actor);
		} else if (auto ct = actor->GetActorRuntimeData().currentCombatTarget.get()) {
			target = ct.get();
		} else {
			throw RpcError("bad_request", "this spell needs a target");
		}
		const float cost = spell->CalculateMagickaCost(actor);
		auto*       av = actor->AsActorValueOwner();
		if (av->GetActorValue(RE::ActorValue::kMagicka) < cost) {
			throw RpcError("failed", std::format("not enough magicka ({:.0f} needed)", cost));
		}
		auto* caster = actor->GetMagicCaster(RE::MagicSystem::CastingSource::kInstant);
		if (!caster) {
			throw RpcError("failed", "you cannot cast right now");
		}
		av->DamageActorValue(RE::ActorValue::kMagicka, cost);
		caster->CastSpellImmediate(spell, false, target, 1.0f, false, 0.0f, actor);
		return Ok({ { "spell", spell->GetName() }, { "target", Describe::Name(target) }, { "magicka_cost", std::round(cost) } });
	}

	json Attack(const json& p)
	{
		auto* actor = Body(p);
		if (!Settings::Get().allowAttack) {
			throw RpcError("disabled", "attacking on command is disabled ([Actions] AllowAttack)");
		}
		auto* target = Targets::Actor(Net::RequireString(p, "target"), actor);
		if (target == actor) {
			throw RpcError("bad_request", "you cannot attack yourself");
		}
		if (target->IsPlayerRef() && !Settings::Get().allowAttackPlayer) {
			throw RpcError("disabled", "attacking the player is disabled ([Actions] AllowAttackPlayer)");
		}
		if (target->IsDead()) {
			throw RpcError("failed", Describe::Name(target) + " is already dead");
		}
		if (Behavior::Restrains(Agents::AgentOf(p), target) && !Net::OptBool(p, "force", false)) {
			throw RpcError("failed", Describe::Name(target) + " is not hostile and your standing orders (restraint) forbid attacking them; pass force to override");
		}
		const bool ok = actor->StartCombat(target);
		return json{ { "ok", ok }, { "target", Describe::Name(target) } };
	}

	json StopCombat(const json& p)
	{
		auto* actor = Body(p);
		actor->StopCombat();
		return Ok();
	}

	// ---------------------------------------------------------------------- gaze
	// Papyrus SetLookAt turns the whole body toward the target and keeps it turned until cleared, so
	// a follower that looked at something behind it ran after the player backwards. The gaze is
	// therefore timed, and paused while the body travels away from what it looks at, or fights.
	namespace
	{
		using Clock = std::chrono::steady_clock;

		struct Gaze
		{
			RE::FormID          actor{ 0 };
			RE::ObjectRefHandle target;
			Clock::time_point   until{ Clock::time_point::max() };
			bool                applied{ false };
			RE::NiPoint3        lastPos{};
			Clock::time_point   stillSince{};
		};
		std::map<std::string, Gaze> g_gaze;  // main thread only

		constexpr auto  kGazeTick = 250ms;
		constexpr float kMovingMps = 0.8f;  // above this the body is travelling
		constexpr float kStillMps = 0.3f;   // below this for kSettle it has stopped
		constexpr auto  kSettle = 750ms;
		constexpr float kAheadCos = 0.5f;  // within 60 degrees of the direction of travel

		void ApplyGaze(RE::Actor* a_actor, RE::TESObjectREFR* a_target, bool a_on)
		{
			if (a_on) {
				Papyrus::CallMethod(a_actor, "Actor", "SetLookAt", RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(a_target), false));
			} else {
				Papyrus::CallMethod(a_actor, "Actor", "ClearLookAt", RE::MakeFunctionArguments());
			}
		}
	}

	void Tick(float)
	{
		static Clock::time_point next{};
		const auto               now = Clock::now();
		if (g_gaze.empty() || now < next) {
			return;
		}
		next = now + kGazeTick;
		constexpr float dt = std::chrono::duration<float>(kGazeTick).count();
		for (auto it = g_gaze.begin(); it != g_gaze.end();) {
			auto& g = it->second;
			auto* actor = RE::TESForm::LookupByID<RE::Actor>(g.actor);
			auto  target = g.target.get();
			if (!actor || actor->IsDead() || !target || target->IsDisabled() || now >= g.until) {
				if (actor && g.applied) {
					ApplyGaze(actor, nullptr, false);  // time is up: eyes front again
				}
				it = g_gaze.erase(it);
				continue;
			}
			const auto  pos = actor->GetPosition();
			const auto  step = pos - g.lastPos;
			const float stepLen = std::hypot(step.x, step.y);
			const float speed = stepLen / kU / dt;
			g.lastPos = pos;
			if (speed < kStillMps) {
				if (g.stillSince == Clock::time_point{}) {
					g.stillSince = now;
				}
			} else {
				g.stillSince = {};
			}

			bool want = g.applied;  // between "travelling" and "stopped": keep what it is doing
			if (actor->IsInCombat()) {
				want = false;  // the combat AI decides where to face
			} else if (speed > kMovingMps) {
				const auto  to = target->GetPosition() - pos;
				const float toLen = std::hypot(to.x, to.y);
				want = toLen < kU || (to.x * step.x + to.y * step.y) / (toLen * stepLen) > kAheadCos;
			} else if (g.stillSince != Clock::time_point{} && now - g.stillSince >= kSettle) {
				want = true;
			}
			if (want != g.applied) {
				ApplyGaze(actor, target.get(), want);
				g.applied = want;
			}
			++it;
		}
	}

	void OnGameLoaded()
	{
		g_gaze.clear();
	}

	json LookAt(const json& p)
	{
		auto*      actor = Body(p);
		const auto agent = Agents::AgentOf(p);
		const auto spec = Net::OptString(p, "target");
		if (spec.empty()) {
			g_gaze.erase(agent);
			ApplyGaze(actor, nullptr, false);
			return Ok({ { "looking_at", nullptr } });
		}
		auto* target = Targets::Ref(spec, actor);
		if (target == actor) {
			throw RpcError("bad_request", "you cannot look at yourself");
		}
		const double seconds = std::clamp(Net::OptNumber(p, "duration_s", 15.0), 0.0, 600.0);
		if (!Papyrus::CallMethod(actor, "Actor", "SetLookAt", RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(target), false))) {
			throw RpcError("failed", "could not set look target");
		}
		Gaze g;
		g.actor = actor->GetFormID();
		g.target = target->GetHandle();
		g.applied = true;
		g.lastPos = actor->GetPosition();
		g.stillSince = Clock::now();
		if (seconds > 0.0) {
			g.until = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(seconds));
		}
		g_gaze[agent] = g;
		json out{ { "looking_at", Describe::Name(target) } };
		out["for_s"] = seconds > 0.0 ? json(std::round(seconds)) : json("until you look elsewhere");
		out["note"] = "while you walk, you stop staring at things behind you and look again once you stop";
		return Ok(std::move(out));
	}

	json Emote(const json& p)
	{
		auto*      actor = Body(p);
		const auto expr = Net::OptString(p, "expression");
		const auto anim = Net::OptString(p, "animation");
		if (expr.empty() && anim.empty()) {
			throw RpcError("bad_request", "give an expression and/or an animation");
		}
		json out = json::object();
		if (!expr.empty()) {
			const auto strength = static_cast<float>(Net::OptNumber(p, "strength", 0.7));
			const auto ms = static_cast<std::uint32_t>(std::clamp(Net::OptNumber(p, "duration_ms", 4000), 0.0, 600000.0));
			if (!Voice::SetExpression(actor, expr, strength, ms)) {
				throw RpcError("bad_request", "unknown expression '" + expr + "' (happy, sad, angry, fear, surprise, puzzled, disgust, neutral)");
			}
			out["expression"] = expr;
		}
		if (!anim.empty()) {
			out["animation"] = Body::PlayGestureOrEvent(actor, Agents::AgentOf(p), anim);
		}
		return Ok(std::move(out));
	}

	json DrawWeapon(const json& p)
	{
		auto*      actor = Body(p);
		const bool draw = Net::OptBool(p, "draw", true);
		actor->DrawWeaponMagicHands(draw);
		return Ok({ { "drawn", draw } });
	}

	json OpenInventory(const json& p)
	{
		auto* actor = Body(p);
		if (!Papyrus::CallMethod(actor, "Actor", "OpenInventory", RE::MakeFunctionArguments(true))) {
			throw RpcError("failed", "could not open the inventory");
		}
		return Ok();
	}

	json Notify(const json& p)
	{
		MainThread::RequireInGame();
		auto text = Net::RequireString(p, "text");
		if (text.size() > 240) {
			text.resize(240);
		}
		RE::SendHUDMessage::ShowHUDMessage(text.c_str(), nullptr, false);
		return Ok();
	}

	json Console(const json& p)
	{
		if (!Settings::Get().allowConsole) {
			throw RpcError("disabled", "console commands are disabled ([Debug] AllowConsole)");
		}
		MainThread::RequireInGame();
		const auto         command = Net::RequireString(p, "command");
		RE::TESObjectREFR* target = nullptr;
		if (const auto t = Net::OptString(p, "target"); !t.empty()) {
			target = Targets::Ref(t, Agents::Get(Agents::AgentOf(p)));
		}
		auto* factory = RE::IFormFactory::GetConcreteFormFactoryByType<RE::Script>();
		auto* script = factory ? factory->Create() : nullptr;
		if (!script) {
			throw RpcError("internal", "could not create a script object");
		}
		logger::warn("console (agent request): {}", command);
		script->SetCommand(command);
		script->CompileAndRun(target);
		delete script;
		return Ok({ { "command", command } });
	}
}
