#include "PCH.h"
#include "Body.h"
#include "Agents.h"
#include "Behavior.h"
#include "Describe.h"
#include "Events.h"
#include "MainThread.h"
#include "Papyrus.h"
#include "Targets.h"

#include "Settings.h"
#include "core/Text.h"
#include "net/Rpc.h"

namespace Game::Body
{
	namespace
	{
		using Net::RpcError;
		constexpr float kU = Targets::kUnitsPerMeter;

		// Vanilla Skyrim.esm idle forms (load index 00 is stable). `exit` marks a pose that lasts
		// until stand(): its exit idle is played to get up.
		struct GestureDef
		{
			const char* name;
			RE::FormID  idle;
			RE::FormID  exit;
			const char* desc;
		};
		constexpr GestureDef kGestures[] = {
			{ "wave", 0x0003EA32, 0, "wave hello" },
			{ "get_attention", 0x0006FF15, 0, "wave someone over" },
			{ "laugh", 0x00075C5F, 0, "laugh" },
			{ "applaud", 0x000D8730, 0, "clap" },
			{ "applaud_sarcastic", 0x000E9312, 0, "a slow, sarcastic clap" },
			{ "cheer", 0x000F7C8C, 0, "raise a fist and cheer" },
			{ "salute", 0x000B240C, 0, "a soldier's salute" },
			{ "attention", 0x000B240B, 0, "snap to attention" },
			{ "bow", 0x000D8734, 0, "a silent bow" },
			{ "nervous", 0x0010ACB1, 0, "fidget nervously" },
			{ "pray", 0x0006F300, 0, "a short standing prayer" },
			{ "point_close", 0x000FFA0C, 0, "point at something nearby" },
			{ "point_far", 0x000B8F1E, 0, "point into the distance" },
			{ "look_far", 0x00075C62, 0, "shade your eyes and look into the distance" },
			{ "examine", 0x00075C3D, 0, "look something over closely" },
			{ "study", 0x000977ED, 0, "study something thoughtfully" },
			{ "wipe_brow", 0x000977EC, 0, "wipe your brow" },
			{ "drink", 0x000FD68B, 0, "take a drink" },
			{ "drink_potion", 0x000D33B0, 0, "drink a potion" },
			{ "warm_hands", 0x000E8642, 0, "warm your hands (by a fire)" },
			{ "cower", 0x000EFC68, 0, "cower in fear" },
			{ "dance", 0x000F7C8A, 0, "a jester's dance" },
			{ "dance2", 0x000F7C8B, 0, "another jester's dance" },
			{ "dance3", 0x00103653, 0, "a third jester's dance" },
			{ "sit_ground", 0x000C4EF9, 0x000C4EFA, "sit cross-legged on the ground (pose)" },
			{ "sit_ledge", 0x000C482B, 0x000C482D, "sit on a ledge, legs dangling (pose)" },
			{ "kneel", 0x000E8E52, 0x000E8E53, "kneel (pose)" },
			{ "kneel_pray", 0x000F11EE, 0x000F11EF, "kneel in prayer (pose)" },
			{ "lean_wall", 0x00052FF8, 0x0005654B, "lean back against a wall; stand with your back to one first (pose)" },
			{ "lean_table", 0x000C4EF6, 0x000C4EF7, "lean on a table or counter (pose)" },
			{ "lean_rail", 0x00070EA0, 0x00070E9F, "lean on a railing (pose)" },
		};

		std::map<std::string, RE::FormID> g_pose;  // agent -> exit idle of the pose they are in
		std::atomic<std::uint32_t>        g_nextAction{ 200000 };

		const GestureDef* FindGesture(std::string_view a_name)
		{
			for (auto& g : kGestures) {
				if (Core::IEquals(g.name, a_name)) {
					return &g;
				}
			}
			return nullptr;
		}

		bool PlayIdle(RE::Actor* a_actor, RE::FormID a_idle)
		{
			auto* idle = RE::TESForm::LookupByID<RE::TESIdleForm>(a_idle);
			auto* proc = a_actor->GetActorRuntimeData().currentProcess;
			if (!idle) {
				return false;
			}
			if (proc && proc->PlayIdle(a_actor, idle, nullptr)) {
				return true;
			}
			// the idle's conditions or parent tree refused it; try its raw animation event
			return !idle->animEventName.empty() && a_actor->NotifyAnimationGraph(idle->animEventName);
		}

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

		void SetWaiting(RE::Actor* a_actor, bool a_wait)
		{
			a_actor->AsActorValueOwner()->SetActorValue(RE::ActorValue::kWaitingForPlayer, a_wait ? 1.f : 0.f);
			a_actor->EvaluatePackage(true, false);
		}

		// ---------------------------------------------------------------------------- furniture
		RE::TESFurniture* SeatOf(RE::TESObjectREFR* a_ref, bool a_lean)
		{
			auto* base = a_ref ? a_ref->GetBaseObject() : nullptr;
			auto* furn = base ? base->As<RE::TESFurniture>() : nullptr;
			if (!furn || furn->workBenchData.benchType.get() != RE::TESFurniture::WorkBenchData::BenchType::kNone) {
				return nullptr;  // not furniture, or a crafting station
			}
			using M = RE::TESFurniture::ActiveMarker;
			return furn->furnFlags.any(a_lean ? M::kCanLean : M::kCanSit) ? furn : nullptr;
		}

		RE::TESObjectREFR* FindSeat(RE::Actor* a_actor, RE::TESObjectREFR* a_near, bool a_lean, float a_radiusM)
		{
			RE::TESObjectREFR* best = nullptr;
			float              bestD = a_radiusM * kU;
			const auto         center = (a_near ? a_near : a_actor)->GetPosition();
			for (auto* ref : Targets::InRange(a_actor, 35.f * kU)) {
				if (!SeatOf(ref, a_lean) || ref->IsDisabled()) {
					continue;
				}
				const float d = ref->GetPosition().GetDistance(center);
				if (d < bestD) {
					best = ref;
					bestD = d;
				}
			}
			return best;
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

		using Clock = std::chrono::steady_clock;
		using SS = RE::SIT_SLEEP_STATE;

		SS SitState(RE::Actor* a_actor)
		{
			auto* st = a_actor->AsActorState();
			return st ? st->GetSitSleepState() : SS::kNormal;
		}

		// In (or getting into) a chair, bench or bed. kRidingMount shares kIsSitting's value, so a
		// rider is excluded explicitly.
		bool Seated(RE::Actor* a_actor)
		{
			if (a_actor->IsOnMount()) {
				return false;
			}
			const auto s = SitState(a_actor);
			return s == SS::kWantToSit || s == SS::kWaitingForSitAnim || s == SS::kIsSitting;
		}

		bool InFurniture(RE::Actor* a_actor)
		{
			return !a_actor->IsOnMount() && SitState(a_actor) != SS::kNormal;
		}

		std::string FurnitureName(RE::Actor* a_actor)
		{
			if (auto f = a_actor->GetOccupiedFurniture().get()) {
				const auto n = Describe::Name(f.get());
				if (!n.empty()) {
					return n;
				}
			}
			return "furniture";
		}

		// A sit request, stepped from the frame callback (main thread only, so no lock). The AI
		// sometimes walks off or refuses the first activation; it gets three tries, re-walking to
		// the seat when it has drifted away, and a late success is still reported.
		struct SitJob
		{
			enum class Phase
			{
				kWalking,
				kArrived,
				kSitting
			};
			std::uint32_t       id{ 0 };
			std::string         agent;
			RE::FormID          actor{ 0 };
			RE::ObjectRefHandle seat;
			std::string         seatName;
			int                 attempt{ 1 };
			Phase               phase{ Phase::kWalking };
			Clock::time_point   deadline{};
			bool                settling{ false };
		};
		std::vector<SitJob> g_sitJobs;

		struct LateSit  // a sit that reported failure but might still happen
		{
			std::uint32_t     id;
			std::string       agent;
			RE::FormID        actor;
			std::string       seatName;
			Clock::time_point until;
		};
		std::vector<LateSit> g_lateSits;

		constexpr int  kSitAttempts = 3;
		constexpr auto kWalkTimeout = 45s;
		constexpr auto kActivateWait = 4s;
		constexpr auto kSettleWait = 6s;

		void WalkToSeat(SitJob& a_job, RE::Actor* a_actor, RE::TESObjectREFR* a_seat)
		{
			a_job.phase = SitJob::Phase::kWalking;
			a_job.deadline = Clock::now() + kWalkTimeout;
			auto*      args = RE::MakeFunctionArguments(static_cast<RE::TESObjectREFR*>(a_seat), 0.0f);
			const auto id = a_job.id;
			const auto attempt = a_job.attempt;
			const bool ok = Papyrus::CallMethod(a_actor, "Actor", "PathToReference", args, [id, attempt](const RE::BSScript::Variable&) {
				SKSE::GetTaskInterface()->AddTask([id, attempt]() {
					for (auto& j : g_sitJobs) {
						if (j.id == id && j.attempt == attempt && j.phase == SitJob::Phase::kWalking) {
							j.phase = SitJob::Phase::kArrived;  // pathing ended (arrived or gave up): try the seat either way
						}
					}
				});
			});
			if (!ok) {
				a_job.phase = SitJob::Phase::kArrived;  // no path; the activation can still walk to the marker
			}
		}

		// true when the job is finished
		bool StepSit(SitJob& j)
		{
			const auto now = Clock::now();
			auto*      actor = RE::TESForm::LookupByID<RE::Actor>(j.actor);
			auto       seatPtr = j.seat.get();
			auto*      seat = seatPtr.get();
			if (!actor || actor->IsDead() || !seat || seat->IsDisabled()) {
				Finished(j.id, j.agent, "sit", false, { { "seat", j.seatName }, { "reason", "the seat or your body is gone" } });
				return true;
			}
			if (actor->IsInCombat()) {
				SetWaiting(actor, false);
				Finished(j.id, j.agent, "sit", false, { { "seat", j.seatName }, { "reason", "a fight started" } });
				return true;
			}
			switch (j.phase) {
			case SitJob::Phase::kWalking:
				if (now >= j.deadline) {
					j.phase = SitJob::Phase::kArrived;  // the walk never reported back; try from here
				}
				return false;
			case SitJob::Phase::kArrived:
				SetWaiting(actor, true);  // stay put instead of being pulled back into following
				seat->ActivateRef(actor, 0, nullptr, 1, false);
				j.phase = SitJob::Phase::kSitting;
				j.deadline = now + kActivateWait;
				j.settling = false;
				return false;
			case SitJob::Phase::kSitting:
				{
					const auto s = SitState(actor);
					if (s == SS::kIsSitting && !actor->IsOnMount()) {
						json extra{ { "seat", FurnitureName(actor) }, { "sitting", true } };
						if (j.attempt > 1) {
							extra["attempts"] = j.attempt;
						}
						Finished(j.id, j.agent, "sit", true, std::move(extra));
						return true;
					}
					if ((s == SS::kWantToSit || s == SS::kWaitingForSitAnim) && !j.settling) {
						j.settling = true;  // on the way into the seat: give the animation time
						j.deadline = std::max(j.deadline, now + kSettleWait);
					}
					if (now < j.deadline) {
						return false;
					}
					if (j.attempt < kSitAttempts) {
						++j.attempt;
						const float d = actor->GetPosition().GetDistance(seat->GetPosition()) / kU;
						logger::info("body: sit attempt {} on {} ({:.1f} m away, state {})", j.attempt, j.seatName, d, static_cast<int>(s));
						if (d > 1.5f) {
							WalkToSeat(j, actor, seat);
						} else {
							j.phase = SitJob::Phase::kArrived;
						}
						return false;
					}
					SetWaiting(actor, false);
					g_lateSits.push_back({ j.id, j.agent, j.actor, j.seatName, now + 90s });
					Finished(j.id, j.agent, "sit", false, { { "seat", j.seatName }, { "sitting", false }, { "attempts", j.attempt },
						{ "hint", "the AI would not use it; a sat_down event follows if you still end up on it. Otherwise try emote with sit_ground or sit_ledge" } });
					return true;
				}
			}
			return true;
		}

		void CancelSits(const std::string& a_agent, const char* a_reason)
		{
			std::erase_if(g_sitJobs, [&](const SitJob& j) {
				if (j.agent != a_agent) {
					return false;
				}
				Finished(j.id, j.agent, "sit", false, { { "seat", j.seatName }, { "reason", a_reason } });
				return true;
			});
			std::erase_if(g_lateSits, [&](const LateSit& l) { return l.agent == a_agent; });
		}

		const SitJob* PendingSit(const std::string& a_agent)
		{
			for (auto& j : g_sitJobs) {
				if (j.agent == a_agent) {
					return &j;
				}
			}
			return nullptr;
		}

		// A pose that was refused because the body was still walking: stop, then try once more.
		struct PoseRetry
		{
			std::uint32_t     id;
			std::string       agent;
			RE::FormID        actor;
			const void*       gesture;
			bool              wasWaiting;
			Clock::time_point at;
		};
		std::vector<PoseRetry> g_poseRetries;

		// ---------------------------------------------------------------------------- spells
		const char* SchoolName(RE::ActorValue a_av)
		{
			switch (a_av) {
			case RE::ActorValue::kAlteration: return "alteration";
			case RE::ActorValue::kConjuration: return "conjuration";
			case RE::ActorValue::kDestruction: return "destruction";
			case RE::ActorValue::kIllusion: return "illusion";
			case RE::ActorValue::kRestoration: return "restoration";
			default: return "";
			}
		}

		const char* DeliveryName(RE::MagicSystem::Delivery a_d)
		{
			using D = RE::MagicSystem::Delivery;
			switch (a_d) {
			case D::kSelf: return "self";
			case D::kTouch: return "touch";
			case D::kAimed: return "aimed";
			case D::kTargetActor: return "target_actor";
			case D::kTargetLocation: return "target_location";
			default: return "other";
			}
		}

		bool HealsOthers(RE::SpellItem* a_spell)
		{
			for (auto* e : a_spell->effects) {
				auto* m = e ? e->baseEffect : nullptr;
				if (!m || m->data.primaryAV != RE::ActorValue::kHealth || m->IsHostile() || m->IsDetrimental()) {
					continue;
				}
				using A = RE::EffectSetting::Archetype;
				const auto arch = m->GetArchetype();
				if (arch != A::kValueModifier && arch != A::kPeakValueModifier && arch != A::kDualValueModifier) {
					continue;
				}
				if (a_spell->GetDelivery() != RE::MagicSystem::Delivery::kSelf || e->effectItem.area > 0) {
					return true;
				}
			}
			return false;
		}

		json SpellRow(RE::Actor* a_actor, RE::SpellItem* s)
		{
			using T = RE::MagicSystem::SpellType;
			const auto type = s->GetSpellType();
			json j{ { "id", Core::FormatFormID(s->GetFormID()) }, { "name", s->GetName() ? s->GetName() : "" },
				{ "kind", type == T::kPower ? "power" : type == T::kLesserPower ? "lesser_power" : type == T::kVoicePower ? "shout" :
						  type == T::kAbility ? "ability" : "spell" } };
			if (type == T::kSpell) {
				j["magicka_cost"] = std::lround(s->CalculateMagickaCost(a_actor));
				j["delivery"] = DeliveryName(s->GetDelivery());
				j["casting"] = s->GetCastingType() == RE::MagicSystem::CastingType::kConcentration ? "concentration" : "fire_and_forget";
				if (const char* school = SchoolName(s->GetAssociatedSkill()); *school) {
					j["school"] = school;
				}
				if (s->IsTwoHanded()) {
					j["two_handed"] = true;
				}
			}
			if (HealsOthers(s)) {
				j["heals_others"] = true;
			}
			if (a_actor->GetEquippedObject(false) == s) {
				j["equipped"] = "right";
			} else if (a_actor->GetEquippedObject(true) == s) {
				j["equipped"] = "left";
			}
			return j;
		}
	}

	// ================================================================================ sit / stand
	json Sit(const json& p)
	{
		auto*      actor = Body(p);
		const auto agent = Agents::AgentOf(p);
		const bool lean = Core::IEquals(Net::OptString(p, "pose", "sit"), "lean");
		const auto spec = Net::OptString(p, "target");

		RE::TESObjectREFR* seat = nullptr;
		if (spec.empty()) {
			seat = FindSeat(actor, nullptr, lean, 8.f);
			if (!seat) {
				throw RpcError("not_found", std::format("nothing to {} on within 8 m; try emote gesture {} instead", lean ? "lean" : "sit",
												lean ? "lean_wall" : "sit_ground"));
			}
		} else {
			auto* ref = Targets::Ref(spec, actor);
			// a crate, log or bench usually carries an invisible seat marker; use the nearest one
			seat = SeatOf(ref, lean) ? ref : FindSeat(actor, ref, lean, 2.5f);
			if (!seat) {
				throw RpcError("not_found", std::format("{} has no {} spot; try emote gesture {} next to it", Describe::Name(ref),
												lean ? "lean" : "sit", lean ? "lean_wall" : "sit_ground"));
			}
		}
		if (seat->GetPosition().GetDistance(actor->GetPosition()) / kU > 35.f) {
			throw RpcError("failed", "that seat is too far away; move closer first");
		}
		if (actor->IsOnMount()) {
			throw RpcError("failed", "you are riding; dismount first");
		}
		if (SitState(actor) == SS::kIsSitting) {
			throw RpcError("failed", "you are already sitting on " + FurnitureName(actor) + "; stand_up first");
		}
		CancelSits(agent, "replaced by a new sit");
		g_pose.erase(agent);
		const auto id = g_nextAction++;
		const auto name = Describe::Name(seat).empty() ? std::string(lean ? "a lean spot" : "a seat") : Describe::Name(seat);
		SitJob job;
		job.id = id;
		job.agent = agent;
		job.actor = actor->GetFormID();
		job.seat = seat->GetHandle();
		job.seatName = name;
		Behavior::BeginAction(agent, actor, id, std::chrono::seconds{ 150 });  // close-follow lets go first
		SetWaiting(actor, true);  // stop following now, so the AI does not drag the body back between tries
		if (seat->GetPosition().GetDistance(actor->GetPosition()) / kU < 1.5f) {
			job.phase = SitJob::Phase::kArrived;
		} else {
			WalkToSeat(job, actor, seat);
		}
		g_sitJobs.push_back(std::move(job));
		json out{ { "action_id", id }, { "seat", Describe::Brief(seat, actor) } };
		out["note"] = "you stay seated (as if told to wait) until stand_up or follow_player; action_finished says whether you sat";
		return out;
	}

	void Tick(float)
	{
		static Clock::time_point next{};
		const auto               now = Clock::now();
		if (now < next) {
			return;
		}
		next = now + 250ms;
		std::erase_if(g_sitJobs, [](SitJob& j) { return StepSit(j); });

		std::erase_if(g_lateSits, [&](const LateSit& l) {
			auto* a = RE::TESForm::LookupByID<RE::Actor>(l.actor);
			if (!a || now >= l.until) {
				return true;
			}
			if (SitState(a) == SS::kIsSitting && !a->IsOnMount()) {
				SetWaiting(a, true);  // what the sit asked for: stay seated
				Events::Log().Push("sat_down", { { "agent", l.agent }, { "seat", FurnitureName(a) }, { "action_id", l.id },
												   { "note", "the earlier sit took effect late; you stay seated until stand_up" } });
				return true;
			}
			return false;
		});

		std::erase_if(g_poseRetries, [&](const PoseRetry& r) {
			if (now < r.at) {
				return false;
			}
			const auto* g = static_cast<const GestureDef*>(r.gesture);
			auto*       a = RE::TESForm::LookupByID<RE::Actor>(r.actor);
			const bool  ok = a && !a->IsDead() && PlayIdle(a, g->idle);
			if (ok) {
				g_pose[r.agent] = g->exit;
			} else if (a && !r.wasWaiting) {
				SetWaiting(a, false);
			}
			json extra{ { "gesture", g->name }, { "played", ok } };
			if (!ok) {
				extra["reason"] = "refused even standing still; this spot or your state does not allow it";
			}
			Finished(r.id, r.agent, "emote", ok, std::move(extra));
			return true;
		});
	}

	json Stand(const json& p)
	{
		auto*      actor = Body(p);
		const auto agent = Agents::AgentOf(p);
		json       out{ { "ok", true } };
		if (PendingSit(agent)) {
			CancelSits(agent, "cancelled by stand_up");
			out["cancelled_sit"] = true;
		}
		std::erase_if(g_poseRetries, [&](const PoseRetry& r) { return r.agent == agent; });
		if (auto it = g_pose.find(agent); it != g_pose.end()) {
			out["left_pose"] = PlayIdle(actor, it->second);
			g_pose.erase(it);
		} else if (Seated(actor)) {
			out["got_up"] = actor->NotifyAnimationGraph("IdleChairExitStart");
		} else {
			actor->NotifyAnimationGraph("IdleForceDefaultState");
		}
		SetWaiting(actor, false);
		return out;
	}

	// ================================================================================ gestures
	json Gestures(const json&)
	{
		json list = json::array();
		for (auto& g : kGestures) {
			list.push_back({ { "name", g.name }, { "what", g.desc }, { "pose", g.exit != 0 } });
		}
		return { { "gestures", list },
			{ "note", "poses last until stand_up; one-shot gestures end on their own. Raw animation event names also work." } };
	}

	void Cancel(const std::string& a_agent)
	{
		CancelSits(a_agent, "cancelled (you were given another order)");
		std::erase_if(g_poseRetries, [&](const PoseRetry& r) { return r.agent == a_agent; });
	}

	json PlayGestureOrEvent(RE::Actor* a_actor, const std::string& a_agent, const std::string& a_name)
	{
		if (const auto* g = FindGesture(a_name)) {
			json j{ { "gesture", g->name }, { "played", false } };
			if (g->exit) {
				j["pose"] = true;
			}
			// Things the idle system refuses outright, said plainly instead of a bare played:false.
			if (a_actor->IsOnMount()) {
				j["reason"] = "not while riding (dismount first)";
				return j;
			}
			if (const auto* sit = PendingSit(a_agent)) {
				j["reason"] = std::format("you are on your way to sit on {} (action {}); stand_up cancels that", sit->seatName, sit->id);
				return j;
			}
			if (InFurniture(a_actor)) {
				const auto s = SitState(a_actor);
				j["reason"] = s == SS::kWantToStand || s == SS::kWantToWake ?
				                  std::string("you are getting up; try again in a moment") :
				                  "you are sitting on " + FurnitureName(a_actor) + "; stand_up first";
				return j;
			}
			if (auto it = g_pose.find(a_agent); it != g_pose.end() && g->exit) {
				j["reason"] = "you are already in a pose; stand_up first";
				return j;
			}
			if (PlayIdle(a_actor, g->idle)) {
				j["played"] = true;
				if (g->exit) {
					g_pose[a_agent] = g->exit;
					SetWaiting(a_actor, true);  // hold the pose instead of walking off mid-way
				}
				return j;
			}
			auto* st = a_actor->AsActorState();
			if (st && st->IsWeaponDrawn()) {
				j["reason"] = "sheathe your weapon first (ready_weapon with draw=false)";
			} else if (a_actor->IsInCombat()) {
				j["reason"] = "not during a fight";
			} else if (st && st->IsSwimming()) {
				j["reason"] = "not while swimming";
			} else if (g->exit && a_actor->IsMoving()) {
				// Following the player: stop, then try the pose again once the body stands still.
				const bool waiting = a_actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kWaitingForPlayer) > 0.f;
				SetWaiting(a_actor, true);
				const auto id = g_nextAction++;
				g_poseRetries.push_back({ id, a_agent, a_actor->GetFormID(), g, waiting, Clock::now() + 1200ms });
				j["retrying"] = true;
				j["action_id"] = id;
				j["reason"] = "you were walking; stopping and trying again (action_finished follows)";
			} else if (a_actor->IsMoving()) {
				j["reason"] = "you were walking; gestures play standing still (wait_here, or try when the player stops)";
			} else {
				j["reason"] = "the idle system refused it here (mid-animation, or this spot does not allow it); try again in a moment";
			}
			return j;
		}
		if (!std::ranges::all_of(a_name, [](unsigned char c) { return std::isalnum(c) || c == '_'; })) {
			throw RpcError("bad_request", "unknown gesture; see gestures for the list");
		}
		const bool accepted = a_actor->NotifyAnimationGraph(RE::BSFixedString(a_name));
		return { { "animation_event", a_name }, { "played", accepted },
			{ "note", accepted ? "raw animation event" : "the animation graph ignored that event; see gestures" } };
	}

	// ================================================================================ spells
	json Spells(const json& p)
	{
		MainThread::RequireInGame();
		auto*      actor = Agents::Require(p, false);
		const bool passive = Net::OptBool(p, "include_passive", false);
		json       list = json::array();
		for (auto* s : Targets::KnownSpells(actor)) {
			const auto t = s->GetSpellType();
			if (!passive && t != RE::MagicSystem::SpellType::kSpell && t != RE::MagicSystem::SpellType::kPower &&
				t != RE::MagicSystem::SpellType::kLesserPower) {
				continue;
			}
			list.push_back(SpellRow(actor, s));
		}
		const float magicka = actor->AsActorValueOwner()->GetActorValue(RE::ActorValue::kMagicka);
		return { { "spells", list }, { "magicka", std::lround(magicka) },
			{ "magicka_max", std::lround(actor->GetActorValueMax(RE::ActorValue::kMagicka)) } };
	}

	json LearnSpell(const json& p)
	{
		auto* actor = Body(p);
		auto  item = Targets::Item(actor, Net::RequireString(p, "tome"));
		auto* book = item.object->As<RE::TESObjectBOOK>();
		if (!book || !book->TeachesSpell() || !book->GetSpell()) {
			throw RpcError("failed", std::string(item.object->GetName()) + " is not a spell tome");
		}
		auto* spell = book->GetSpell();
		if (actor->HasSpell(spell)) {
			return { { "ok", false }, { "already_known", spell->GetName() }, { "note", "the tome is kept" } };
		}
		if (!actor->AddSpell(spell)) {
			throw RpcError("failed", std::string("could not learn ") + spell->GetName());
		}
		actor->RemoveItem(book, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);  // a learned tome crumbles, as for the player
		Events::Log().Push("spell_learned", { { "agent", Agents::AgentOf(p) }, { "spell", spell->GetName() } });
		json j{ { "ok", true }, { "learned", SpellRow(actor, spell) } };
		j["note"] = "equip it with use_item (hand=left/right) or cast it with cast_spell";
		return j;
	}
}
