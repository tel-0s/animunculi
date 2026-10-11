#include "PCH.h"
#include "Targets.h"

#include "core/Text.h"
#include "net/Rpc.h"

namespace Game::Targets
{
	namespace
	{
		const char* NameOf(RE::TESObjectREFR* a_ref)
		{
			const char* n = a_ref->GetDisplayFullName();
			return n ? n : "";
		}

		// 0 exact, 1 prefix, 2 substring, -1 no match
		int MatchTier(std::string_view a_name, std::string_view a_spec)
		{
			if (a_name.empty()) {
				return -1;
			}
			if (Core::IEquals(a_name, a_spec)) {
				return 0;
			}
			if (a_name.size() >= a_spec.size() && Core::IEquals(a_name.substr(0, a_spec.size()), a_spec)) {
				return 1;
			}
			return Core::IContains(a_name, a_spec) ? 2 : -1;
		}

		bool Usable(RE::TESObjectREFR* a_ref)
		{
			return a_ref && !a_ref->IsDisabled() && !a_ref->IsDeleted() && !a_ref->IsMarkedForDeletion() && a_ref->GetBaseObject();
		}
	}

	std::vector<RE::TESObjectREFR*> InRange(RE::TESObjectREFR* a_origin, float a_radius)
	{
		std::vector<std::pair<float, RE::TESObjectREFR*>> found;
		auto*                                             tes = RE::TES::GetSingleton();
		if (!tes || !a_origin) {
			return {};
		}
		const auto origin = a_origin->GetPosition();
		tes->ForEachReferenceInRange(a_origin, a_radius, [&](RE::TESObjectREFR* a_ref) {
			if (a_ref && a_ref != a_origin && Usable(a_ref) && a_ref->Is3DLoaded()) {
				found.emplace_back(a_ref->GetPosition().GetDistance(origin), a_ref);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::ranges::sort(found, {}, &std::pair<float, RE::TESObjectREFR*>::first);
		std::vector<RE::TESObjectREFR*> out;
		out.reserve(found.size());
		for (auto& [d, r] : found) {
			out.push_back(r);
		}
		return out;
	}

	RE::TESObjectREFR* Ref(std::string_view a_spec, RE::Actor* a_self, float a_searchRadius)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto spec = Core::Lower(a_spec);
		if (spec.empty()) {
			throw Net::RpcError("bad_request", "empty target");
		}
		if (spec == "self" || spec == "me") {
			if (!a_self) {
				throw Net::RpcError("not_bound", "'self' needs a bound body");
			}
			return a_self;
		}
		if (spec == "player") {
			return player;
		}
		if (spec == "crosshair") {
			auto* pick = RE::CrosshairPickData::GetSingleton();
			auto  ref = pick ? pick->GetActiveTarget().get() : RE::NiPointer<RE::TESObjectREFR>{};
			if (!ref) {
				throw Net::RpcError("not_found", "the player is not pointing at anything");
			}
			return ref.get();
		}
		if (spec == "combat_target") {
			if (!a_self) {
				throw Net::RpcError("not_bound", "'combat_target' needs a bound body");
			}
			auto t = a_self->GetActorRuntimeData().currentCombatTarget.get();
			if (!t) {
				throw Net::RpcError("not_found", "not fighting anyone");
			}
			return t.get();
		}

		RE::TESObjectREFR* origin = a_self ? static_cast<RE::TESObjectREFR*>(a_self) : player;

		if (const auto id = Core::ParseFormID(a_spec)) {
			auto* form = RE::TESForm::LookupByID(*id);
			if (!form) {
				throw Net::RpcError("not_found", "no form " + std::string(a_spec));
			}
			if (auto* ref = form->As<RE::TESObjectREFR>()) {
				if (!Usable(ref)) {
					throw Net::RpcError("not_found", std::string(a_spec) + " is disabled or deleted");
				}
				return ref;
			}
			// A base form: the closest loaded reference of it.
			for (auto* ref : InRange(origin, a_searchRadius)) {
				if (ref->GetBaseObject() == form) {
					return ref;
				}
			}
			throw Net::RpcError("not_found", "no loaded reference of " + std::string(a_spec) + " nearby");
		}

		RE::TESObjectREFR* best = nullptr;
		int                bestTier = 99;
		if (MatchTier(NameOf(player), a_spec) == 0) {
			return player;
		}
		for (auto* ref : InRange(origin, a_searchRadius)) {  // closest first
			const int tier = MatchTier(NameOf(ref), a_spec);
			if (tier >= 0 && tier < bestTier) {
				best = ref;
				bestTier = tier;
				if (tier == 0) {
					break;
				}
			}
		}
		if (!best) {
			throw Net::RpcError("not_found", "nothing called '" + std::string(a_spec) + "' nearby");
		}
		return best;
	}

	RE::Actor* Actor(std::string_view a_spec, RE::Actor* a_self, float a_searchRadius)
	{
		auto* ref = Ref(a_spec, a_self, a_searchRadius);
		auto* actor = ref->As<RE::Actor>();
		if (!actor) {
			throw Net::RpcError("not_found", std::string(NameOf(ref)) + " is not an actor");
		}
		return actor;
	}

	InventoryItem Item(RE::TESObjectREFR* a_owner, std::string_view a_spec)
	{
		const auto    id = Core::ParseFormID(a_spec);
		InventoryItem best;
		int           bestTier = 99;
		auto          inv = a_owner->GetInventory();
		for (auto& [obj, data] : inv) {
			auto& [count, entry] = data;
			if (!obj || count <= 0) {
				continue;
			}
			int tier = -1;
			if (id) {
				tier = obj->GetFormID() == *id ? 0 : -1;
			} else {
				const char* n = obj->GetName();
				tier = MatchTier(n ? n : "", a_spec);
			}
			if (tier >= 0 && tier < bestTier) {
				best = { obj, count, entry && entry->IsWorn() };
				bestTier = tier;
			}
		}
		if (!best.object) {
			throw Net::RpcError("not_found", std::string(NameOf(a_owner)) + " has no '" + std::string(a_spec) + "'");
		}
		return best;
	}

	std::vector<RE::SpellItem*> KnownSpells(RE::Actor* a_actor)
	{
		std::vector<RE::SpellItem*> out;
		auto add = [&](RE::SpellItem* s) {
			if (s && std::ranges::find(out, s) == out.end()) {
				out.push_back(s);
			}
		};
		if (auto* base = a_actor->GetActorBase(); base) {
			if (auto* list = base->GetSpellList(); list && list->spells) {
				for (std::uint32_t i = 0; i < list->numSpells; ++i) {
					add(list->spells[i]);
				}
			}
		}
		if (auto* race = a_actor->GetRace(); race && race->actorEffects && race->actorEffects->spells) {
			for (std::uint32_t i = 0; i < race->actorEffects->numSpells; ++i) {
				add(race->actorEffects->spells[i]);
			}
		}
		for (auto* s : a_actor->GetActorRuntimeData().addedSpells) {
			add(s);
		}
		return out;
	}

	RE::SpellItem* Spell(RE::Actor* a_actor, std::string_view a_spec)
	{
		const auto     id = Core::ParseFormID(a_spec);
		RE::SpellItem* best = nullptr;
		int            bestTier = 99;
		for (auto* s : KnownSpells(a_actor)) {
			int tier = -1;
			if (id) {
				tier = s->GetFormID() == *id ? 0 : -1;
			} else {
				const char* n = s->GetName();
				tier = MatchTier(n ? n : "", a_spec);
			}
			if (tier >= 0 && tier < bestTier) {
				best = s;
				bestTier = tier;
			}
		}
		if (!best) {
			throw Net::RpcError("not_found", std::string(a_actor->GetDisplayFullName()) + " does not know '" + std::string(a_spec) + "'");
		}
		return best;
	}

	Ownership OwnershipFor(RE::TESObjectREFR* a_ref, RE::Actor* a_actor)
	{
		if (!a_ref) {
			return Ownership::kFree;
		}
		if (a_actor) {
			if (auto* owner = a_ref->GetOwner()) {
				if (owner->Is(RE::FormType::NPC)) {
					if (owner == a_actor->GetActorBase() || owner == a_actor->GetTemplateBase()) {
						return Ownership::kMine;
					}
				} else if (owner->Is(RE::FormType::Faction)) {
					if (a_actor->IsInFaction(static_cast<RE::TESFaction*>(owner))) {
						return Ownership::kMine;
					}
				}
			}
		}
		return a_ref->IsCrimeToActivate() ? Ownership::kOther : Ownership::kFree;
	}

	std::string OwnerName(RE::TESObjectREFR* a_ref)
	{
		auto* owner = a_ref ? a_ref->GetOwner() : nullptr;
		if (!owner) {
			return {};
		}
		const char* n = owner->GetName();
		if (n && *n) {
			return n;
		}
		const char* edid = owner->GetFormEditorID();
		return edid && *edid ? edid : Core::FormatFormID(owner->GetFormID());
	}

	RE::Actor* MountOf(RE::Actor* a_actor)
	{
		if (!a_actor || !a_actor->IsOnMount()) {
			return nullptr;
		}
		RE::NiPointer<RE::Actor> mount;
		return a_actor->GetMount(mount) && mount ? mount.get() : nullptr;
	}

	bool Rideable(RE::Actor* a_mount, RE::Actor* a_rider)
	{
		if (!a_mount || !a_mount->IsAMount() || a_mount->IsDead() || a_mount->IsDisabled() || !a_mount->Is3DLoaded()) {
			return false;
		}
		if (a_mount->IsBeingRidden()) {
			RE::NiPointer<RE::Actor> rider;
			return a_mount->GetMountedBy(rider) && rider.get() == a_rider;
		}
		return true;
	}
}
